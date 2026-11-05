# R8 shrinks, optimizes and obfuscates the release build. Crash traces are mapped back to the
# original names with the build's mapping.txt, which CI attaches to each GitHub release.

# The engine's JNI entry points are kept by core/consumer-rules.pro.

# Meta Quest build: the native OpenXR host binds QuestActivity's natives by name and calls onNativeExit back.
-keep class com.example.projectm.visualizer.quest.QuestActivity {
    native <methods>;
    void onNativeExit(java.lang.String);
}
