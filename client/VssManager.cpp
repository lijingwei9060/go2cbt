#include "VssManager.h"
#include "Logger.h"


namespace VssSnapshot
{

	VssManager::VssManager()
		: m_initialized(false)
		, m_snapshotCreated(false)
		, m_pVss(nullptr)
	{
		memset(&m_snapshotSetId, 0, sizeof(m_snapshotSetId));
	}

	VssManager::~VssManager()
	{
		if (m_initialized)
		{
			Cleanup();
		}
	}

	// ============================================================
	// 初始化 VSS
	// ============================================================
	bool VssManager::Initialize()
	{
		if (m_initialized)
		{
			return true;
		}

		// Step 1: 初始化 COM（VSS 依赖 COM）
		HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
		if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
		{
			// S_FALSE: 已被同一线程初始化（不同并发模式），仍可用
			// RPC_E_CHANGED_MODE: 该线程已用不同模式初始化，VSS 可能仍可用
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] CoInitializeEx failed: 0x%08X", hr);
			LOG_WARNING(msg);
		}

		// Step 2: 创建 IVssBackupComponents 实例
		hr = CreateVssBackupComponents(&m_pVss);
		if (FAILED(hr))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] CreateVssBackupComponents failed: 0x%08X", hr);
			LOG_ERROR(msg);
			m_lastError = L"Failed to create VSS backup components";
			return false;
		}

		// Step 3: 初始化备份组件
		hr = m_pVss->InitializeForBackup();
		if (FAILED(hr))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] InitializeForBackup failed: 0x%08X", hr);
			LOG_ERROR(msg);

			m_pVss->Release();
			m_pVss = nullptr;
			m_lastError = L"Failed to initialize VSS for backup";
			return false;
		}

		// Step 4: 获取 Writer Metadata（记录参与的 Writer 信息）
		IVssAsync* pAsync = nullptr;
		hr = m_pVss->GatherWriterMetadata(&pAsync);
		if (SUCCEEDED(hr) && pAsync)
		{
			pAsync->Wait();
			HRESULT hrResult = 0;
			pAsync->QueryStatus(&hrResult, nullptr);
			pAsync->Release();

			if (FAILED(hrResult))
			{
				wchar_t msg[256];
				swprintf_s(msg, L"[VssManager] GatherWriterMetadata failed: 0x%08X", hrResult);
				LOG_WARNING(msg);
			}
			else
			{
				LOG_INFO(L"[VssManager] Writer metadata gathered successfully");
			}
		}

		// Step 5: 获取 VSS Writer 数量（用于日志）
		UINT cWriters = 0;
		hr = m_pVss->GetWriterMetadataCount(&cWriters);
		if (SUCCEEDED(hr))
		{
			wchar_t msg[128];
			swprintf_s(msg, L"[VssManager] %u VSS writer(s) detected", cWriters);
			LOG_INFO(msg);

			// 打印每个 Writer 的名称
			for (UINT i = 0; i < cWriters; i++)
			{
				VSS_ID idWriter = {};
				IVssExamineWriterMetadata* pMetadata = nullptr;
				if (SUCCEEDED(m_pVss->GetWriterMetadata(i, &idWriter, &pMetadata)))
				{
					VSS_ID idInstance = {}, idWriterId = {};
					BSTR bstrWriterName = nullptr;
					if (SUCCEEDED(pMetadata->GetIdentity(&idInstance, &idWriterId, &bstrWriterName, nullptr, nullptr)))
					{
						wchar_t writerMsg[512];
						swprintf_s(writerMsg, L"[VssManager]   Writer[%u]: %s", i, bstrWriterName);
						LOG_INFO(writerMsg);
						SysFreeString(bstrWriterName);
					}
					pMetadata->Release();
				}
			}
		}

		m_initialized = true;
		LOG_INFO(L"[VssManager] VSS initialized successfully");
		return true;
	}

	// ============================================================
	// 将卷添加到快照集
	// ============================================================
	bool VssManager::AddVolumeToSnapshotSet(const std::wstring& volumeGuid, VSS_ID& snapshotSetId)
	{
		if (!m_initialized || !m_pVss)
		{
			m_lastError = L"VSS not initialized";
			return false;
		}

		// 将 \\?\Volume{GUID}\ 转换为 VSS 接受的格式
		std::wstring volPath = volumeGuid;
		// 如果以反斜杠结尾，去掉（VSS 要求无尾反斜杠）
		if (!volPath.empty() && volPath.back() == L'\\')
		{
			volPath.pop_back();
		}

		VSS_ID snapshotId = {};
		HRESULT hr = m_pVss->AddToSnapshotSet(
			const_cast<LPWSTR>(volPath.c_str()),
			GUID_NULL,                // 使用系统默认 Provider
			&snapshotId
		);

		if (FAILED(hr))
		{
			wchar_t msg[512];
			swprintf_s(msg, L"[VssManager] AddToSnapshotSet failed for %s: 0x%08X",
				volPath.c_str(), hr);
			LOG_ERROR(msg);
			m_lastError = L"Failed to add volume to snapshot set";
			return false;
		}

		// 记录映射关系
		m_volumeToSnapshotId[volumeGuid] = snapshotId;
		// 记录快照集 ID（第一次添加时设置）
		if (m_snapshotMap.empty())
		{
			m_snapshotSetId = snapshotId;
			// m_snapshotSetId 作为 snapshot set ID（同一次 AddToSnapshotSet 的所有 volume 共享）
		}

		wchar_t msg[512];
		swprintf_s(msg, L"[VssManager] Volume added to snapshot set: %s", volPath.c_str());
		LOG_INFO(msg);

		snapshotSetId = snapshotId;
		return true;
	}

	// ============================================================
	// 设置备份状态
	// ============================================================
	bool VssManager::SetBackupState()
	{
		if (!m_initialized || !m_pVss)
		{
			return false;
		}

		// 通知 Writer：这是全量备份（也可以是增量/差异备份）
		HRESULT hr = m_pVss->SetBackupState(
			true,              // bSelectComponents: false = 备份整个卷
			true,              // bBackupBootableSystemState: 包含引导卷
			VSS_BT_FULL,       // 备份类型 = 全量
			false              // bPartialFileSupport
		);

		if (FAILED(hr))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] SetBackupState failed: 0x%08X", hr);
			LOG_WARNING(msg);
			// 继续执行，不致命
		}
		else
		{
			LOG_INFO(L"[VssManager] Backup state set: VSS_BT_FULL");
		}

		return true;
	}

	// ============================================================
	// 创建快照（所有卷在同一时刻冻结）
	// ============================================================
	bool VssManager::DoSnapshotSet()
	{
		if (!m_initialized || !m_pVss)
		{
			m_lastError = L"VSS not initialized";
			return false;
		}

		if (m_volumeToSnapshotId.empty())
		{
			m_lastError = L"No volumes added to snapshot set";
			return false;
		}

		// Step 1: Prepare for backup（通知所有 Writer 准备备份）
		IVssAsync* pPrepareAsync = nullptr;
		HRESULT hr = m_pVss->PrepareForBackup(&pPrepareAsync);
		if (SUCCEEDED(hr) && pPrepareAsync)
		{
			wchar_t msg[128];
			swprintf_s(msg, L"[VssManager] PrepareForBackup started for %zu volume(s)",
				m_volumeToSnapshotId.size());
			LOG_INFO(msg);

			if (!WaitForAsyncOperation(pPrepareAsync, L"PrepareForBackup"))
			{
				wchar_t msg2[256];
				swprintf_s(msg2, L"[VssManager] PrepareForBackup wait failed for %s", m_lastError.c_str());
				LOG_ERROR(msg2);
				// 继续尝试 DoSnapshotSet（某些 Writer 失败不影响快照创建）
			}
		}
		else
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] PrepareForBackup failed: 0x%08X", hr);
			LOG_WARNING(msg);
		}

		// Step 2: 创建快照
		IVssAsync* pDoSnapshotAsync = nullptr;
		hr = m_pVss->DoSnapshotSet(&pDoSnapshotAsync);

		if (FAILED(hr))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] DoSnapshotSet failed: 0x%08X", hr);
			LOG_ERROR(msg);
			m_lastError = L"Failed to create VSS snapshot";
			return false;
		}

		LOG_INFO(L"[VssManager] DoSnapshotSet started, waiting for completion...");

		if (!WaitForAsyncOperation(pDoSnapshotAsync, L"DoSnapshotSet"))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] DoSnapshotSet wait failed: %s", m_lastError.c_str());
			LOG_ERROR(msg);
			return false;
		}

		// Step 3: 获取快照属性（快照设备路径等）
		if (!GetSnapshotProperties())
		{
			LOG_ERROR(L"[VssManager] Failed to get snapshot properties");
			return false;
		}

		m_snapshotCreated = true;

		wchar_t msg[256];
		swprintf_s(msg, L"[VssManager] Snapshot set created successfully (%zu volumes)",
			m_snapshotMap.size());
		LOG_INFO(msg);

		return true;
	}

	// ============================================================
	// 获取卷的快照设备路径
	// ============================================================
	bool VssManager::GetSnapshotDevicePath(const std::wstring& volumeGuid, std::wstring& snapshotPath)
	{
		auto it = m_snapshotMap.find(volumeGuid);
		if (it == m_snapshotMap.end())
		{
			wchar_t msg[512];
			swprintf_s(msg, L"[VssManager] Snapshot device path not found for volume %s",
				volumeGuid.c_str());
			LOG_WARNING(msg);
			snapshotPath.clear();
			return false;
		}

		snapshotPath = it->second.SnapshotDevicePath;
		return true;
	}

	// ============================================================
	// 查询单个卷的快照属性
	// ============================================================
	bool VssManager::QuerySnapshotProperties(const std::wstring& volumeGuid)
	{
		if (!m_pVss)
		{
			return false;
		}

		auto it = m_volumeToSnapshotId.find(volumeGuid);
		if (it == m_volumeToSnapshotId.end())
		{
			return false;
		}

		VSS_SNAPSHOT_PROP prop = {};
		VSS_ID snapshotId = it->second;

		// 注意：GetSnapshotProperties 需要一个未发布的快照
		// 在 DoSnapshotSet 之后调用
		HRESULT hr = m_pVss->GetSnapshotProperties(snapshotId, &prop);
		if (FAILED(hr))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] GetSnapshotProperties failed for snapshotId: 0x%08X", hr);
			LOG_WARNING(msg);
			return false;
		}

		wchar_t msg[512];
		swprintf_s(msg, L"[VssManager] Snapshot properties: device=%ls",
			prop.m_pwszSnapshotDeviceObject ? prop.m_pwszSnapshotDeviceObject : L"(null)");
		LOG_INFO(msg);

		VssFreeSnapshotProperties(&prop);
		return true;
	}

	// ============================================================
	// 清理 VSS 快照和资源
	// ============================================================
	void VssManager::Cleanup()
	{
		m_lastError.clear();

		if (m_pVss && m_snapshotCreated)
		{
			// Step 1: 通知 Writer 备份已完成
			IVssAsync* pCompleteAsync = nullptr;
			HRESULT hr = m_pVss->BackupComplete(&pCompleteAsync);
			if (SUCCEEDED(hr) && pCompleteAsync)
			{
				pCompleteAsync->Wait();

				HRESULT hrResult = 0;
				pCompleteAsync->QueryStatus(&hrResult, nullptr);
				pCompleteAsync->Release();

				if (FAILED(hrResult))
				{
					wchar_t msg[256];
					swprintf_s(msg, L"[VssManager] BackupComplete failed: 0x%08X", hrResult);
					LOG_WARNING(msg);
				}
				else
				{
					LOG_INFO(L"[VssManager] BackupComplete signaled to writers");
				}
			}

			// Step 2: 删除快照（不保留）
			// 快照集 ID 用于批量删除
			LONG deletedSnapshots = 0;
			VSS_ID nonDeleted = {};
			hr = m_pVss->DeleteSnapshots(m_snapshotSetId,
				VSS_OBJECT_SNAPSHOT_SET,
				true,            // bForceDelete
				&deletedSnapshots,
				&nonDeleted
			);

			if (SUCCEEDED(hr))
			{
				wchar_t msg[256];
				swprintf_s(msg, L"[VssManager] %ld snapshot(s) deleted", deletedSnapshots);
				LOG_INFO(msg);
			}
			else
			{
				wchar_t msg[256];
				swprintf_s(msg, L"[VssManager] DeleteSnapshots failed: 0x%08X", hr);
				LOG_WARNING(msg);
			}

			m_snapshotCreated = false;
		}

		// Step 3: 释放 IVssBackupComponents
		if (m_pVss)
		{
			m_pVss->Release();
			m_pVss = nullptr;
		}

		// Step 4: 反初始化 COM
		CoUninitialize();

		m_initialized = false;
		m_snapshotMap.clear();
		m_volumeToSnapshotId.clear();
		m_providerName.clear();

		LOG_INFO(L"[VssManager] Cleanup completed");
	}

	// ============================================================
	// 等待异步操作完成
	// ============================================================
	bool VssManager::WaitForAsyncOperation(IVssAsync* pAsync, const wchar_t* operationName)
	{
		if (!pAsync)
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] %s: null async pointer", operationName);
			LOG_WARNING(msg);
			m_lastError = msg;
			return false;
		}

		// 等待异步操作完成
		HRESULT hrWait = pAsync->Wait();
		if (FAILED(hrWait))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] %s: IVssAsync::Wait() failed: 0x%08X", operationName, hrWait);
			LOG_ERROR(msg);
			m_lastError = msg;
			pAsync->Release();
			return false;
		}

		// 查询操作结果
		HRESULT hrResult = 0;
		HRESULT hrQuery = pAsync->QueryStatus(&hrResult, nullptr);
		pAsync->Release();

		if (FAILED(hrQuery))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] %s: QueryStatus failed: 0x%08X", operationName, hrQuery);
			LOG_ERROR(msg);
			m_lastError = msg;
			return false;
		}

		if (FAILED(hrResult))
		{
			wchar_t msg[256];
			swprintf_s(msg, L"[VssManager] %s completed with error: 0x%08X", operationName, hrResult);
			LOG_WARNING(msg);
			m_lastError = msg;
			return false;
		}

		wchar_t msg[256];
		swprintf_s(msg, L"[VssManager] %s completed successfully", operationName);
		LOG_INFO(msg);
		return true;
	}

	// ============================================================
	// 获取快照属性列表（遍历所有快照并填充 m_snapshotMap）
	// ============================================================
	bool VssManager::GetSnapshotProperties()
	{
		if (!m_pVss)
		{
			return false;
		}

		// 遍历所有已添加到快照集的卷，获取每个卷的快照设备路径
		for (const auto& pair : m_volumeToSnapshotId)
		{
			const std::wstring& volumeGuid = pair.first;
			VSS_ID snapshotId = pair.second;

			VSS_SNAPSHOT_PROP prop = {};
			HRESULT hr = m_pVss->GetSnapshotProperties(snapshotId, &prop);

			if (FAILED(hr))
			{
				wchar_t msg[512];
				swprintf_s(msg, L"[VssManager] GetSnapshotProperties failed for volume %s: 0x%08X",
					volumeGuid.c_str(), hr);
				LOG_WARNING(msg);
				continue;
			}

			// 记录 Provider 名称（第一个卷）
			if (m_providerName.empty())
			{
				m_providerName = L"Microsoft Software Shadow Copy provider";
			}

			// 填充快照设备路径
			// 格式: \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN
			std::wstring snapshotDevice;
			if (prop.m_pwszSnapshotDeviceObject)
			{
				snapshotDevice = prop.m_pwszSnapshotDeviceObject;
			}

			AddSnapshotInfo(snapshotId, m_snapshotSetId, volumeGuid, snapshotDevice);

			wchar_t msg[512];
			swprintf_s(msg, L"[VssManager] Snapshot device for %s: %s",
				volumeGuid.c_str(),
				snapshotDevice.empty() ? L"(not available)" : snapshotDevice.c_str());
			LOG_INFO(msg);

			VssFreeSnapshotProperties(&prop);
		}

		return !m_snapshotMap.empty();
	}

	// ============================================================
	// 添加快照信息到内部映射
	// ============================================================
	void VssManager::AddSnapshotInfo(VSS_ID snapshotId, VSS_ID snapshotSetId,
		const std::wstring& originalVolume, const std::wstring& snapshotDevice)
	{
		SnapshotVolumeInfo info;
		info.OriginalVolumeGuid = originalVolume;
		info.SnapshotDevicePath = snapshotDevice;
		info.SnapshotId = snapshotId;
		info.SnapshotSetId = snapshotSetId;

		m_snapshotMap[originalVolume] = info;
	}

	// ============================================================
	// 检查 VSS 服务是否可用（静态方法）
	// ============================================================
	bool VssManager::IsVssAvailable()
	{
		IVssBackupComponents* pTest = nullptr;
		HRESULT hr = CreateVssBackupComponents(&pTest);
		if (FAILED(hr))
		{
			return false;
		}

		// 尝试初始化（可选，只需判断接口是否可创建）
		pTest->Release();
		return true;
	}

} // namespace VssSnapshot
