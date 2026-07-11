#include "PrivilegeManager.h"

#include <iostream>


namespace BackupSecurity
{


    bool PrivilegeManager::IsAdministrator()
    {

        BOOL result = FALSE;



        PSID adminGroup = nullptr;



        SID_IDENTIFIER_AUTHORITY ntAuthority =
            SECURITY_NT_AUTHORITY;



        if (!AllocateAndInitializeSid(
            &ntAuthority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &adminGroup))
        {
            return false;
        }




        CheckTokenMembership(
            nullptr,
            adminGroup,
            &result
        );



        FreeSid(adminGroup);



        return result == TRUE;

    }





    void PrivilegeManager::ShowPrivilegeError()
    {

        std::wcerr
            <<
            L"====================================\n"
            <<
            L"错误：当前程序没有管理员权限\n"
            <<
            L"请右键选择【以管理员身份运行】\n"
            <<
            L"====================================\n";

    }


}