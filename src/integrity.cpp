// wwe2k16 - native half of the anti-repackage check (r18, Integrity.java).
//
// Holds the brand id (volvisionmods.com app id) and returns its FNV-1a hash. Integrity.enforceWithNative compares this
// with the hash it computes from VolvisionUpdateChannel.APP_ID in Java: the two copies must match, so the branding in
// the Java code and in the compiled library cannot be edited independently. Removing this file makes the native symbol
// vanish, which Integrity.java also treats as tamper.

#include <cstdint>
#include <jni.h>

namespace {
constexpr char kBrandId[] = "YAehU4cLlL5Lv4ts7HB0";

int32_t Fnv1a(const char* s) {
  uint32_t h = 0x811c9dc5u;
  for (const char* p = s; *p; ++p) {
    h ^= static_cast<uint8_t>(*p);
    h *= 0x01000193u;
  }
  return static_cast<int32_t>(h);
}
}  // namespace

extern "C" JNIEXPORT jint JNICALL
Java_com_savanindurkar7_wwe2k16_Integrity_nativeBrandToken(JNIEnv*, jclass) {
  return Fnv1a(kBrandId);
}
