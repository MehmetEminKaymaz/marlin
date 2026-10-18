// macOS'a özgü küçük parça (Objective-C): kütüphanelerin istisnalarını yakalamak.
//
// MoltenVK (macOS'ta Vulkan) Metal'in Objective-C nesneleriyle çalışır.
// Sanal makinelerin Metal aygıtı (ör. GitHub'ın Mac makineleri:
// AppleParavirtDevice) bazı çağrıları desteklemez; MoltenVK aygıtı
// tanırken Objective-C istisnası alır ve yakalamaz. C'de istisna
// yakalanamaz: program çökerdi. Burada yakalanır, GPU "yok" sayılır ve
// @GPU fonksiyonlar CPU'da çalışır.

#include "platform.h"

int mr_call_guarded(int (*fn)(void)) {
    @try {
        return fn();
    } @catch (id exception) {
        (void)exception;
        return 0;
    }
}
