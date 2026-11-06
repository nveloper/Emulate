#include "frontend.h"

#include <android/native_window_jni.h>
#include <jni.h>

#include <string>

namespace {

std::string toString(JNIEnv* env, jstring str) {
    if (!str) return {};
    const char* chars = env->GetStringUTFChars(str, nullptr);
    std::string result(chars);
    env->ReleaseStringUTFChars(str, chars);
    return result;
}

Frontend& fe() { return Frontend::instance(); }

}  // namespace

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
    jclass cls = env->FindClass("com/emulate/app/NativeBridge");
    if (!cls) return JNI_ERR;
    Frontend& f = fe();
    f.vm = vm;
    f.bridgeClass = static_cast<jclass>(env->NewGlobalRef(cls));
    f.onVideoSizeMethod = env->GetStaticMethodID(cls, "onVideoSize", "(II)V");
    f.onMessageMethod = env->GetStaticMethodID(cls, "onMessage", "(Ljava/lang/String;)V");
    f.onNetplayStatusMethod = env->GetStaticMethodID(cls, "onNetplayStatus", "(IILjava/lang/String;)V");
    f.onRumbleMethod = env->GetStaticMethodID(cls, "onRumble", "(I)V");
    return JNI_VERSION_1_6;
}

JNIEXPORT jstring JNICALL Java_com_emulate_app_NativeBridge_start(
    JNIEnv* env, jobject, jstring corePath, jstring romPath, jstring savePath, jstring rtcPath,
    jstring systemDir, jstring saveDir, jstring slot2Rom, jstring slot2Save, jobjectArray optionKeys,
    jobjectArray optionValues, jint language) {
    LaunchConfig config;
    config.corePath = toString(env, corePath);
    config.romPath = toString(env, romPath);
    config.savePath = toString(env, savePath);
    config.rtcPath = toString(env, rtcPath);
    config.systemDir = toString(env, systemDir);
    config.saveDir = toString(env, saveDir);
    config.slot2RomPath = toString(env, slot2Rom);
    config.slot2SavePath = toString(env, slot2Save);
    config.language = static_cast<unsigned>(language);
    jsize count = optionKeys ? env->GetArrayLength(optionKeys) : 0;
    for (jsize i = 0; i < count; i++) {
        auto key = static_cast<jstring>(env->GetObjectArrayElement(optionKeys, i));
        auto value = static_cast<jstring>(env->GetObjectArrayElement(optionValues, i));
        config.options[toString(env, key)] = toString(env, value);
        env->DeleteLocalRef(key);
        env->DeleteLocalRef(value);
    }
    std::string error = fe().start(config);
    return error.empty() ? nullptr : env->NewStringUTF(error.c_str());
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_stop(JNIEnv*, jobject) { fe().stop(); }

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setPaused(JNIEnv*, jobject, jboolean paused) {
    fe().setPaused(paused);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setSpeed(JNIEnv*, jobject, jfloat speed) {
    fe().setSpeed(speed);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setSpeedAudio(JNIEnv*, jobject, jboolean enabled) {
    fe().setSpeedAudio(enabled);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setVsyncPacing(JNIEnv*, jobject, jboolean enabled) {
    fe().setVsyncPacing(enabled);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setVideoFilter(JNIEnv*, jobject, jint filter) {
    fe().setVideoFilter(filter);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setGameRect(JNIEnv*, jobject, jint x, jint y, jint w, jint h) {
    fe().setGameRect(x, y, w, h);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setAmbient(JNIEnv*, jobject, jboolean enabled) {
    fe().setAmbient(enabled);
}

JNIEXPORT jintArray JNICALL Java_com_emulate_app_NativeBridge_captureFrame(JNIEnv* env, jobject) {
    std::vector<uint32_t> pixels;
    unsigned width = 0, height = 0;
    if (!fe().captureFrame(pixels, width, height)) return nullptr;
    // [width, height, pixels...]
    jintArray result = env->NewIntArray(static_cast<jsize>(pixels.size() + 2));
    if (!result) return nullptr;
    jint size[2] = {static_cast<jint>(width), static_cast<jint>(height)};
    env->SetIntArrayRegion(result, 0, 2, size);
    env->SetIntArrayRegion(result, 2, static_cast<jsize>(pixels.size()), reinterpret_cast<const jint*>(pixels.data()));
    return result;
}

JNIEXPORT jfloatArray JNICALL Java_com_emulate_app_NativeBridge_getStats(JNIEnv* env, jobject) {
    float values[6];
    fe().stats(values);
    jfloatArray result = env->NewFloatArray(6);
    env->SetFloatArrayRegion(result, 0, 6, values);
    return result;
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setAudioEnabled(JNIEnv*, jobject, jboolean enabled) {
    fe().setAudioEnabled(enabled);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setSurface(JNIEnv* env, jobject, jobject surface) {
    fe().setWindow(surface ? ANativeWindow_fromSurface(env, surface) : nullptr);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setViewSize(JNIEnv*, jobject, jint w, jint h) {
    fe().setViewSize(w, h);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setButtons(JNIEnv*, jobject, jint mask) {
    fe().setButtons(static_cast<uint32_t>(mask));
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setPointer(JNIEnv*, jobject, jint x, jint y, jboolean pressed) {
    fe().setPointer(x, y, pressed);
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setOption(JNIEnv* env, jobject, jstring key, jstring value) {
    fe().setOption(toString(env, key), toString(env, value));
}

JNIEXPORT jboolean JNICALL Java_com_emulate_app_NativeBridge_saveState(JNIEnv* env, jobject, jstring path) {
    return fe().saveState(toString(env, path));
}

JNIEXPORT jboolean JNICALL Java_com_emulate_app_NativeBridge_loadState(JNIEnv* env, jobject, jstring path) {
    return fe().loadState(toString(env, path));
}

JNIEXPORT jstring JNICALL Java_com_emulate_app_NativeBridge_netplayHost(JNIEnv* env, jobject, jstring title, jstring tag) {
    std::string error = fe().netplayHost(toString(env, title), toString(env, tag));
    return error.empty() ? nullptr : env->NewStringUTF(error.c_str());
}

JNIEXPORT jstring JNICALL Java_com_emulate_app_NativeBridge_netplayJoin(JNIEnv* env, jobject, jstring address, jstring tag) {
    std::string error = fe().netplayJoin(toString(env, address), toString(env, tag));
    return error.empty() ? nullptr : env->NewStringUTF(error.c_str());
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_netplayLeave(JNIEnv*, jobject) { fe().netplayLeave(); }

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_setCheats(JNIEnv* env, jobject, jobjectArray codes) {
    std::vector<std::string> list;
    jsize count = codes ? env->GetArrayLength(codes) : 0;
    for (jsize i = 0; i < count; i++) {
        auto code = static_cast<jstring>(env->GetObjectArrayElement(codes, i));
        list.push_back(toString(env, code));
        env->DeleteLocalRef(code);
    }
    fe().setCheats(std::move(list));
}

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_reset(JNIEnv*, jobject) { fe().reset(); }

JNIEXPORT void JNICALL Java_com_emulate_app_NativeBridge_flushSaves(JNIEnv*, jobject) { fe().flushSaves(); }

}  // extern "C"
