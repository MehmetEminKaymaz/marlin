// std.time — takvim zamanı ve bekleme (bkz. std/time.marl).

#include "marlin_rt.h"
#include "platform.h"

// 1 Ocak 1970'ten beri geçen saniye (kesirli). Süre ölçmek için yerleşik
// time() daha uygundur: o saat geri gitmez.
double marlin_time_now(void) { return mr_wall_seconds(); }

void marlin_time_sleep(double seconds) { mr_sleep_seconds(seconds); }
