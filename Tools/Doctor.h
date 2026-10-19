#ifndef MARLIN_DOCTOR_H
#define MARLIN_DOCTOR_H

namespace marlin {

/// `marlin doctor`: kurulumu denetler ve eksikleri kurulum önerileriyle
/// yazar. Küçük test programlarını derleyicinin kendisiyle derleyip
/// çalıştırır; böylece kullanıcının programlarında göreceği sonucu raporlar.
/// Bir program derlenemiyorsa (hata) 1, değilse (uyarılar olsa da) 0 döner.
int runDoctor(const char* argv0);

} // namespace marlin

#endif // MARLIN_DOCTOR_H
