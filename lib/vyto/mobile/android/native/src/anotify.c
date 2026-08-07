/* anotify.c — local notifications.
 *
 * An up-call on the Actions instance the Activity already bound, like
 * ahaptics.c: a notification is a platform action the app delegates to the
 * system, which is exactly what that class is for, and reusing it costs no new
 * binding, no new Java class and no new JNI entry.
 *
 * "Local" is the whole scope. Push (FCM) needs the firebase-messaging AAR,
 * which is Maven resolution, which is Gradle, and this build path deliberately
 * has none — see ANDROID.md. What a Gradle-free app can do is post its own
 * notifications, which covers alarms, timers, downloads and background results.
 *
 * Both arms, since this file compiles for every target.
 */

#include <stdint.h>

#ifdef __ANDROID__

#include "vyto_android.h"

int32_t vta_notify(int32_t id, const char *title, const char *text, int32_t ongoing) {
    JNIEnv *env = vta_env();
    jobject acts = vta_actions();
    if (!env || !acts) return -1;
    jclass cls = (*env)->GetObjectClass(env, acts);
    if (!cls) return -1;
    jmethodID m = (*env)->GetMethodID(env, cls, "notify",
                                      "(ILjava/lang/String;Ljava/lang/String;Z)I");
    int32_t rc = -1;
    if (m) {
        jstring t = (*env)->NewStringUTF(env, title ? title : "");
        jstring x = (*env)->NewStringUTF(env, text ? text : "");
        rc = (int32_t)(*env)->CallIntMethod(env, acts, m, (jint)id, t, x,
                                            ongoing ? JNI_TRUE : JNI_FALSE);
        (*env)->DeleteLocalRef(env, t);
        (*env)->DeleteLocalRef(env, x);
    }
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); rc = -1; }
    return rc;
}

void vta_notify_cancel(int32_t id) {
    JNIEnv *env = vta_env();
    jobject acts = vta_actions();
    if (!env || !acts) return;
    jclass cls = (*env)->GetObjectClass(env, acts);
    if (!cls) return;
    jmethodID m = (*env)->GetMethodID(env, cls, "cancelNotification", "(I)V");
    if (m) (*env)->CallVoidMethod(env, acts, m, (jint)id);
    (*env)->DeleteLocalRef(env, cls);
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
}

#else

int32_t vta_notify(int32_t id, const char *title, const char *text, int32_t ongoing) {
    (void)id; (void)title; (void)text; (void)ongoing;
    return -1;
}
void vta_notify_cancel(int32_t id) { (void)id; }

#endif
