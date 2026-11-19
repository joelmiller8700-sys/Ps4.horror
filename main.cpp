#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>

int main(void) {
    // تهيئة خدمة المستخدم والدراعة للتأكد من عمل المكتبات الأساسية
    sceUserServiceInitialize(NULL);
    scePadInit();

    int userId = 0;
    sceUserServiceGetInitialUser(&userId);
    int padHandle = scePadOpen(userId, 0, 0, NULL);

    // حلقة بسيطة تعمل لمدة 5 ثواني أو حتى تضغط زر X (Cross)
    for (int i = 0; i < 300; i++) {
        if (padHandle >= 0) {
            OrbisPadData padData;
            if (scePadReadState(padHandle, &padData) == 0) {
                if (padData.buttons & ORBIS_PAD_BUTTON_CROSS) {
                    break;
                }
            }
        }
        // انتظار حوالي 16 ملي ثانية (60 فريم بالثانية)
        sceKernelUsleep(16666);
    }

    // إبقاء التطبيق حياً في الخلفية لكي لا يعطي خطأ خروج مفاجئ على PS4
    for (;;) {
        sceKernelUsleep(1000000);
    }

    return 0;
}
