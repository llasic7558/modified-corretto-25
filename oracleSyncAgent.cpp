/*
 * Oracle Sync Agent - JVMTI agent that detects application main() entry
 * and signals EpsilonGC to start oracle tracking.
 */

#include <jvmti.h>
#include <jni.h>
#include <string.h>
#include <stdio.h>
#include <dlfcn.h>

typedef void (*oracle_start_fn)();
static oracle_start_fn g_oracle_start = nullptr;
static jvmtiEnv* g_jvmti = nullptr;
static bool g_main_detected = false;

static bool is_system_class(const char* class_sig) {
    if (class_sig == nullptr) return true;
    return (strncmp(class_sig, "Ljava/", 6) == 0 ||
            strncmp(class_sig, "Ljavax/", 7) == 0 ||
            strncmp(class_sig, "Ljdk/", 5) == 0 ||
            strncmp(class_sig, "Lsun/", 5) == 0 ||
            strncmp(class_sig, "Lcom/sun/", 9) == 0 ||
            strncmp(class_sig, "Lorg/graalvm/", 13) == 0);
}

static void JNICALL MethodEntry(jvmtiEnv* jvmti_env, JNIEnv* jni_env,
                                 jthread thread, jmethodID method) {
    if (g_main_detected) return;

    char* method_name = nullptr;
    char* method_sig = nullptr;

    jvmtiError err = jvmti_env->GetMethodName(method, &method_name, &method_sig, nullptr);
    if (err != JVMTI_ERROR_NONE || method_name == nullptr) return;

    if (strcmp(method_name, "main") == 0 &&
        strcmp(method_sig, "([Ljava/lang/String;)V") == 0) {

        jclass declaring_class;
        err = jvmti_env->GetMethodDeclaringClass(method, &declaring_class);
        if (err == JVMTI_ERROR_NONE) {
            char* class_sig = nullptr;
            err = jvmti_env->GetClassSignature(declaring_class, &class_sig, nullptr);

            if (err == JVMTI_ERROR_NONE && class_sig != nullptr) {
                if (!is_system_class(class_sig)) {
                    fprintf(stderr, "[OracleSync] Application main() detected: %s\n", class_sig);

                    if (g_oracle_start != nullptr) {
                        g_oracle_start();
                        fprintf(stderr, "[OracleSync] Signaled EpsilonGC oracle to start\n");
                    } else {
                        fprintf(stderr, "[OracleSync] WARNING: epsilon_oracle_signal_app_start not found\n");
                    }

                    g_main_detected = true;
                    jvmti_env->SetEventNotificationMode(JVMTI_DISABLE,
                        JVMTI_EVENT_METHOD_ENTRY, nullptr);
                }
                jvmti_env->Deallocate((unsigned char*)class_sig);
            }
        }
    }

    jvmti_env->Deallocate((unsigned char*)method_name);
    jvmti_env->Deallocate((unsigned char*)method_sig);
}

static void JNICALL VMInit(jvmtiEnv* jvmti_env, JNIEnv* jni_env, jthread thread) {
    fprintf(stderr, "[OracleSync] VM initialized, looking for oracle signal function\n");
    g_oracle_start = (oracle_start_fn)dlsym(RTLD_DEFAULT, "epsilon_oracle_signal_app_start");
    if (g_oracle_start == nullptr) {
        fprintf(stderr, "[OracleSync] WARNING: signal function not found (is EpsilonOracleMode enabled?)\n");
    }
}

extern "C" {

JNIEXPORT jint JNICALL Agent_OnLoad(JavaVM* vm, char* options, void* reserved) {
    fprintf(stderr, "[OracleSync] Agent loading...\n");

    jint rc = vm->GetEnv((void**)&g_jvmti, JVMTI_VERSION_1_0);
    if (rc != JNI_OK || g_jvmti == nullptr) {
        fprintf(stderr, "[OracleSync] ERROR: Unable to get JVMTI environment\n");
        return JNI_ERR;
    }

    jvmtiCapabilities caps;
    memset(&caps, 0, sizeof(caps));
    caps.can_generate_method_entry_events = 1;

    jvmtiError err = g_jvmti->AddCapabilities(&caps);
    if (err != JVMTI_ERROR_NONE) {
        fprintf(stderr, "[OracleSync] ERROR: Failed to add capabilities: %d\n", err);
        return JNI_ERR;
    }

    jvmtiEventCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.MethodEntry = &MethodEntry;
    callbacks.VMInit = &VMInit;

    err = g_jvmti->SetEventCallbacks(&callbacks, sizeof(callbacks));
    if (err != JVMTI_ERROR_NONE) {
        fprintf(stderr, "[OracleSync] ERROR: Failed to set callbacks: %d\n", err);
        return JNI_ERR;
    }

    g_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_VM_INIT, nullptr);
    g_jvmti->SetEventNotificationMode(JVMTI_ENABLE, JVMTI_EVENT_METHOD_ENTRY, nullptr);

    fprintf(stderr, "[OracleSync] Agent loaded, waiting for main()\n");
    return JNI_OK;
}

JNIEXPORT void JNICALL Agent_OnUnload(JavaVM* vm) {
    fprintf(stderr, "[OracleSync] Agent unloading\n");
}

}
