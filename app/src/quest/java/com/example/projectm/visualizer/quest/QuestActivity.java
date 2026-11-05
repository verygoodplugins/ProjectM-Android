package com.example.projectm.visualizer.quest;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.media.AudioFormat;
import android.media.AudioRecord;
import android.media.MediaRecorder;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;
import android.view.WindowManager;
import android.widget.Toast;
import java.io.File;

/**
 * Immersive host for Meta Quest 3S / 3 (docs/quest/README.md). The OpenXR session and projectM live
 * in native code (core/src/main/cpp/quest_xr.cpp); this activity starts them, follows the activity
 * lifecycle and feeds them the headset microphone.
 */
public final class QuestActivity extends Activity {
    private static final String TAG = "ProjectMQuest";
    private static final int SAMPLE_RATE = 48000;
    private static final int REQUEST_AUDIO = 1;

    static {
        System.loadLibrary("projectmtv");
    }

    private static native void nativeStart(Activity activity, AssetManager assets, String textureDir);
    private static native void nativeSetResumed(boolean resumed);
    /** Mono float PCM, -1..1; may be called from any thread. */
    private static native void nativePushPcm(float[] samples, int count);
    private static native void nativeStop();

    private MicCapture mic;
    private boolean resumed;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        nativeStart(this, getAssets(), new File(getFilesDir(), "textures").getPath());
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(new String[] {Manifest.permission.RECORD_AUDIO}, REQUEST_AUDIO);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        resumed = true;
        nativeSetResumed(true);
        startMic();
    }

    @Override
    protected void onPause() {
        resumed = false;
        nativeSetResumed(false);
        stopMic();  // never listen while the app is not in front
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        stopMic();
        nativeStop();
        super.onDestroy();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        if (requestCode != REQUEST_AUDIO) return;
        if (results.length > 0 && results[0] == PackageManager.PERMISSION_GRANTED) {
            startMic();
        } else {
            Toast.makeText(this, "Microphone access is needed to react to sound", Toast.LENGTH_LONG).show();
        }
    }

    /** Called from the native render thread when the XR session ended or could not start. */
    void onNativeExit(String reason) {
        runOnUiThread(() -> {
            if (reason != null) Toast.makeText(this, "ProjectM Quest: " + reason, Toast.LENGTH_LONG).show();
            finish();
        });
    }

    private void startMic() {
        if (!resumed || mic != null) return;
        if (checkSelfPermission(Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) return;
        mic = new MicCapture();
        mic.start();
    }

    private void stopMic() {
        if (mic == null) return;
        mic.shutdown();
        mic = null;
    }

    /** Blocking AudioRecord reads on the default input (the headset microphones), pushed as float PCM. */
    private static final class MicCapture extends Thread {
        // Headset mics are quiet and pick up room noise: gate what is below the noise floor and keep the
        // rest near a steady level with a slow gain (quick to back off on loud input).
        private static final float NOISE_GATE = 0.004f;
        private static final float TARGET_PEAK = 0.7f;
        private static final float MIN_GAIN = 1f;
        private static final float MAX_GAIN = 32f;

        private volatile boolean running = true;
        private float gain = 4f;

        MicCapture() {
            super("QuestMicCapture");
        }

        void shutdown() {
            running = false;
            try {
                join(500);  // reads return within ~20 ms
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }

        @Override
        public void run() {
            Process.setThreadPriority(Process.THREAD_PRIORITY_AUDIO);
            int minimum = AudioRecord.getMinBufferSize(SAMPLE_RATE, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_FLOAT);
            if (minimum <= 0) {
                Log.w(TAG, "Float microphone input unavailable");
                return;
            }
            AudioRecord record = new AudioRecord(MediaRecorder.AudioSource.MIC, SAMPLE_RATE,
                    AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_FLOAT, Math.max(minimum, 16384));
            try {
                if (record.getState() != AudioRecord.STATE_INITIALIZED) {
                    Log.w(TAG, "Microphone did not initialize");
                    return;
                }
                record.startRecording();
                float[] pcm = new float[1024];
                while (running) {
                    int count = record.read(pcm, 0, pcm.length, AudioRecord.READ_BLOCKING);
                    if (count <= 0) {
                        Log.w(TAG, "Microphone read failed: " + count);
                        return;
                    }
                    condition(pcm, count);
                    nativePushPcm(pcm, count);
                }
            } finally {
                record.release();
            }
        }

        private void condition(float[] pcm, int count) {
            float peak = 0f;
            for (int i = 0; i < count; i++) peak = Math.max(peak, Math.abs(pcm[i]));
            if (peak < NOISE_GATE) {
                java.util.Arrays.fill(pcm, 0, count, 0f);
                return;
            }
            if (peak * gain > TARGET_PEAK) gain = Math.max(MIN_GAIN, gain * 0.9f);
            else gain = Math.min(MAX_GAIN, gain * 1.002f);
            for (int i = 0; i < count; i++) pcm[i] = Math.max(-1f, Math.min(1f, pcm[i] * gain));
        }
    }
}
