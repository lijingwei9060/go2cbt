#pragma once
#include <windows.h>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <string>
#include <vector>
#include <map>


namespace VssSnapshot
{

//
// 快照卷信息
// 由 VssManager::DoSnapshotSet 填充
//
struct SnapshotVolumeInfo
{
	std::wstring OriginalVolumeGuid;   // 原始卷 GUID (Volume{GUID} 路径)
	std::wstring SnapshotDevicePath;   // 快照设备路径 (例: GLOBALROOT Device HarddiskVolumeShadowCopyN)
	VSS_ID SnapshotId;                 // 快照 ID（用于后续删除）
	VSS_ID SnapshotSetId;              // 快照集 ID
};

//
// VSS 备份阶段枚举（供回调使用）
//
enum class VssBackupPhase
{
	Prepare,
	Freeze,
	Thaw,
	PostSnapshot,
	BackupComplete,
	Shutdown
};

//
// VssManager: VSS 快照管理模块
//
// 职责：
// 1. 初始化 COM + IVssBackupComponents 接口
// 2. 将文件系统卷添加到快照集
// 3. 创建一致性快照（同一时刻冻结所有卷）
// 4. 获取快照后的可读卷设备路径
// 5. 备份完成后清理快照
//
// 使用流程：
//   VssManager vss;
//   vss.Initialize();
//   for (each filesystem partition) {
//       vss.AddVolumeToSnapshotSet(volumeGuid, snapshotSetId);
//   }
//   vss.DoSnapshotSet();
//   for (each volume) {
//       vss.GetSnapshotDevicePath(volumeGuid, snapshotPath);
//       // 从 snapshotPath 读取数据
//   }
//   vss.Cleanup();
//
class VssManager
{

public:

	VssManager();
	~VssManager();

	//
	// 初始化 VSS：创建 IVssBackupComponents 实例
	// 必须在调用其他方法前调用
	// 返回 true 表示初始化成功
	//
	bool Initialize();

	//
	// 将卷添加到快照集
	// volumeGuid: \\?\Volume{GUID}\  格式的卷路径
	// snapshotSetId: [输出] 快照集 ID
	// 返回 true 表示添加成功
	//
	bool AddVolumeToSnapshotSet(const std::wstring& volumeGuid, VSS_ID& snapshotSetId);

	//
	// 设置备份状态（通知 Writer）
	// 必须在 AddVolumeToSnapshotSet 之后、DoSnapshotSet 之前调用
	//
	bool SetBackupState();

	//
	// 创建快照：所有已添加的卷在同一时刻冻结
	// 调用后可通过 GetSnapshotDevicePath 获取快照设备路径
	// 返回 true 表示快照创建成功
	//
	bool DoSnapshotSet();

	//
	// 获取卷的快照设备路径
	// volumeGuid: 原始卷 GUID（与 AddVolumeToSnapshotSet 传入的一致）
	// snapshotPath: [输出] 快照设备路径
	//   格式: \\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN
	// 返回 true 表示获取成功
	//
	bool GetSnapshotDevicePath(const std::wstring& volumeGuid, std::wstring& snapshotPath);

	//
	// 检查单个卷的快照属性（Provider ID、Snapshot 属性等）
	// 用于调试和确认快照状态
	//
	bool QuerySnapshotProperties(const std::wstring& volumeGuid);

	//
	// 清理：通知 Writer 备份完成，删除快照，释放接口
	// 备份结束后必须调用
	//
	void Cleanup();

	//
	// 查询是否已成功初始化
	//
	bool IsInitialized() const { return m_initialized; }

	//
	// 获取 VSS Provider 名称（用于日志）
	//
	std::wstring GetProviderName() const { return m_providerName; }

	//
	// 检查 VSS 服务是否可用
	//
	static bool IsVssAvailable();

	//
	// 获取初始化过程中的错误消息
	//
	std::wstring GetLastError() const { return m_lastError; }

private:

	//
	// 等待异步操作完成
	// 在 PrepareForBackup 和 DoSnapshotSet 后调用
	//
	bool WaitForAsyncOperation(IVssAsync* pAsync, const wchar_t* operationName, DWORD timeoutMs = 600000);

	//
	// 获取快照属性列表
	//
	bool GetSnapshotProperties();

	//
	// 添加快照卷信息到内部映射
	//
	void AddSnapshotInfo(VSS_ID snapshotId, VSS_ID snapshotSetId,
		const std::wstring& originalVolume, const std::wstring& snapshotDevice);

	bool m_initialized;
	bool m_snapshotCreated;

	IVssBackupComponents* m_pVss;

	GUID m_snapshotSetId;

	std::wstring m_providerName;
	std::wstring m_lastError;

	// 原始卷 GUID → 快照信息 映射
	std::map<std::wstring, SnapshotVolumeInfo> m_snapshotMap;

	// 原始卷 GUID → Snapshot ID 映射（用于 GetSnapshotProperties）
	std::map<std::wstring, VSS_ID> m_volumeToSnapshotId;
};

} // namespace VssSnapshot
