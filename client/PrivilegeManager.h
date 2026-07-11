#pragma once
#include <windows.h>


namespace BackupSecurity
{


    class PrivilegeManager
    {

    public:


        /*
            判断当前进程是否管理员权限

        */
        static bool IsAdministrator();



        /*
            如果不是管理员

            输出提示

        */
        static void ShowPrivilegeError();



    };

}