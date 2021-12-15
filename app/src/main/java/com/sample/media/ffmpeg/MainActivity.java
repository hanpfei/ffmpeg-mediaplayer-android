/*
 * Copyright 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package com.sample.media.ffmpeg;

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioRecord;
import android.net.Uri;
import android.os.Bundle;
import androidx.annotation.NonNull;
import androidx.core.app.ActivityCompat;

import android.util.Log;
import android.view.Menu;
import android.view.MenuItem;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.widget.Button;
import android.widget.SeekBar;
import android.widget.TextView;
import android.widget.Toast;

public class MainActivity extends Activity
        implements ActivityCompat.OnRequestPermissionsResultCallback {
    private static final int AUDIO_ECHO_REQUEST = 0;
    private static final int READ_EXTERNAL_STORAGE_REQUEST = 1;
    private static final int WRITE_EXTERNAL_STORAGE_REQUEST = 2;

    private SurfaceView surfaceView;
    private SurfaceHolder surfaceHolder;
    private Button   controlButton;
    private TextView statusView;

    private String  nativeSampleRate;
    private String  nativeSampleBufSize;

    private boolean supportRecording;
    private Boolean isPlaying = false;

    private SeekBar playerProgressSeekBar;
    private TextView curPlayerProgressTV;
    private TextView playerUrlTV;
    private float playerProgress;
    private long playerHandle = 0;
    private float position = 0.0f;

    private Thread renderThread;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        surfaceView = (SurfaceView)findViewById(R.id.surfaceView);
        surfaceHolder = surfaceView.getHolder();

        controlButton = (Button)findViewById((R.id.capture_control_button));
        statusView = (TextView)findViewById(R.id.statusView);
        queryNativeAudioParameters();

        playerUrlTV = (TextView)findViewById(R.id.player_url);
        playerProgressSeekBar = (SeekBar)findViewById(R.id.progressSeekBar);
        curPlayerProgressTV = (TextView)findViewById(R.id.curPlayerProgress);
        playerProgress = (float)playerProgressSeekBar.getProgress() / playerProgressSeekBar.getMax();
        playerProgressSeekBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override
            public void onProgressChanged(SeekBar seekBar, int progress, boolean fromUser) {
                float curVal = (float)progress / seekBar.getMax();
                curPlayerProgressTV.setText(String.format("%s", curVal));
                setSeekBarPromptPosition(playerProgressSeekBar, curPlayerProgressTV);
                if (!fromUser)
                    return;

                playerProgress = curVal;
                if (playerHandle > 0) {
                    playerSeek(playerHandle, playerProgress);
                }
            }
            @Override
            public void onStartTrackingTouch(SeekBar seekBar) {}
            @Override
            public void onStopTrackingTouch(SeekBar seekBar) {}
        });
        playerProgressSeekBar.post(new Runnable() {
            @Override
            public void run() {
                setSeekBarPromptPosition(playerProgressSeekBar, curPlayerProgressTV);
            }
        });

        // initialize native audio system
        updateNativeAudioUI();

        if (supportRecording) {
            createSLEngine(
                    Integer.parseInt(nativeSampleRate),
                    Integer.parseInt(nativeSampleBufSize),
                    1,
                    0.1f);
        }
    }

    private void setSeekBarPromptPosition(SeekBar seekBar, TextView label) {
        float thumbX = (float)seekBar.getProgress()/ seekBar.getMax() *
                              seekBar.getWidth() + seekBar.getX();
        label.setX(thumbX - label.getWidth()/2.0f);
    }

    @Override
    protected void onDestroy() {
        if (supportRecording) {
            if (isPlaying) {
                stopPlay();
            }
            deleteSLEngine();
            isPlaying = false;
        }
        super.onDestroy();
    }

    @Override
    public boolean onCreateOptionsMenu(Menu menu) {
        // Inflate the menu; this adds items to the action bar if it is present.
        getMenuInflater().inflate(R.menu.menu_main, menu);
        return true;
    }

    @Override
    public boolean onOptionsItemSelected(MenuItem item) {
        // Handle action bar item clicks here. The action bar will
        // automatically handle clicks on the Home/Up button, so long
        // as you specify a parent activity in AndroidManifest.xml.
        int id = item.getItemId();

        //noinspection SimplifiableIfStatement
        if (id == R.id.action_settings) {
            return true;
        }

        return super.onOptionsItemSelected(item);
    }

    private void startEcho(Object surface) {
        if(!supportRecording){
            return;
        }
        if (!isPlaying) {
            if(!createSLBufferQueueAudioPlayer()) {
                statusView.setText(getString(R.string.player_error_msg));
                return;
            }
            if(!createAudioRecorder()) {
                deleteSLBufferQueueAudioPlayer();
                statusView.setText(getString(R.string.recorder_error_msg));
                return;
            }
            startPlay(surface);   // startPlay() triggers startRecording()
            statusView.setText(getString(R.string.echoing_status_msg));
        } else {
            stopPlay();  // stopPlay() triggers stopRecording()
            updateNativeAudioUI();
            deleteAudioRecorder();
            deleteSLBufferQueueAudioPlayer();
        }
        isPlaying = !isPlaying;
    }

    public void onSelectFile(View view) {
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) !=
                                               PackageManager.PERMISSION_GRANTED) {
            statusView.setText(getString(R.string.request_permission_status_msg));
            ActivityCompat.requestPermissions(
                    this,
                    new String[] { Manifest.permission.RECORD_AUDIO },
                    AUDIO_ECHO_REQUEST);
            return;
        }
        Intent intent = new Intent(Intent.ACTION_GET_CONTENT);
        intent.setType("*/*");//设置类型，我这里是任意类型，任意后缀的可以这样写。
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        startActivityForResult(intent,1);
    }

    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (resultCode == Activity.RESULT_OK) {//是否选择，没选择就不会继续
            Uri uri = data.getData();//得到uri，后面就是将uri转化成file的过程。
            String img_path = ContentUriUtils.getPath(this, uri);
            if (img_path != null) {
                playerUrlTV.setText(img_path);
            }
        }
    }

    public void onPlayerOpen(View view) {
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.READ_EXTERNAL_STORAGE) !=
                PackageManager.PERMISSION_GRANTED) {
            statusView.setText("READ_EXTERNAL_STORAGE Permission...");
            ActivityCompat.requestPermissions(
                    this,
                    new String[] { Manifest.permission.READ_EXTERNAL_STORAGE },
                    READ_EXTERNAL_STORAGE_REQUEST);
            return;
        }
        if (ActivityCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE) !=
                PackageManager.PERMISSION_GRANTED) {
            statusView.setText("WRITE_EXTERNAL_STORAGE Permission...");
            ActivityCompat.requestPermissions(
                    this,
                    new String[] { Manifest.permission.WRITE_EXTERNAL_STORAGE },
                    WRITE_EXTERNAL_STORAGE_REQUEST);
            return;
        }
        if (playerHandle > 0) {
            Log.i("AUDIO_ECHO", "The media player has been opened");
        } else {
            String url = "/sdcard/Movies/trailer.mp4";
            final String path = playerUrlTV.getText().toString();
            if (path != null && path.length() > 0) {
                url = path;
            }
            if (playerProgress > 0.02) {
                playerHandle = playerOpenAt(url, playerProgress);
            } else {
                playerHandle = playerOpen(url);
            }
        }
    }

    public void onPlayerPlay(View view) {
        if (playerHandle > 0) {
            startEcho(surfaceHolder.getSurface());
            if (renderThread == null) {
                renderThread = new Thread() {
                    @Override
                    public void run() {
                        final int interval_step = 2;
                        long interval = 30;
                        long last_update_interval_time = System.currentTimeMillis();
                        int last_video_frames = 0;
                        long play_start_time = System.currentTimeMillis();

                        boolean last_get_frame_failed = false;
                        while (playerHandle > 0) {
                            try {
                                Thread.sleep(interval);
                            } catch (InterruptedException e) {
                                e.printStackTrace();
                            }
                            long now = System.currentTimeMillis();
                            int video_frames = playerRenderVideo(playerHandle, surfaceHolder.getSurface());
                            if (video_frames < 0) {
                                if (((now - play_start_time) < 70000) && !last_get_frame_failed) {
                                    interval += interval_step;
                                }
                                last_get_frame_failed = true;
                                continue;
                            }
                            last_get_frame_failed = false;
                            if (last_video_frames == 0 && video_frames > 0) {
                                last_video_frames = video_frames;
                            }

                            long time_diff = now - last_update_interval_time;
                            if (time_diff > 1000) {
                                int video_frames_diff = video_frames - 10;
                                if (video_frames_diff < -5 || video_frames < 5) {
                                    interval += interval_step;
                                    last_video_frames = video_frames;
                                    last_update_interval_time = now;
                                } else if (video_frames_diff > 5 && video_frames > 10) {
                                    interval -= interval_step;
                                    last_video_frames = video_frames;
                                    last_update_interval_time = now;
                                    if (interval < 20) {
                                        interval = 20;
                                    }
                                }
                            }
                        }
                        renderThread = null;
                    }
                };
                renderThread.start();
            }
        } else {
            Log.w("AUDIO_ECHO", "Invalid player handle " + playerHandle + " for demuxOneFrame");
        }
    }

    public void onPlayerPause(View view) {
        if (playerHandle > 0) {
            playerPause(playerHandle);
        } else {
            Log.w("AUDIO_ECHO", "Invalid player handle " + playerHandle + " for pause");
        }
    }

    public void onPlayerClose(View view) {
        if (playerHandle > 0) {
            playerClose(playerHandle);
            playerHandle = 0;
        } else {
            Log.w("AUDIO_ECHO", "Invalid player handle " + playerHandle + " for close");
        }
    }

    public void getLowLatencyParameters(View view) {
        updateNativeAudioUI();
    }

    private void queryNativeAudioParameters() {
        supportRecording = true;
        AudioManager myAudioMgr = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        if(myAudioMgr == null) {
            supportRecording = false;
            return;
        }
        nativeSampleRate  =  myAudioMgr.getProperty(AudioManager.PROPERTY_OUTPUT_SAMPLE_RATE);
        nativeSampleBufSize =myAudioMgr.getProperty(AudioManager.PROPERTY_OUTPUT_FRAMES_PER_BUFFER);

        // hardcoded channel to mono: both sides -- C++ and Java sides
        int recBufSize = AudioRecord.getMinBufferSize(
                Integer.parseInt(nativeSampleRate),
                AudioFormat.CHANNEL_IN_MONO,
                AudioFormat.ENCODING_PCM_16BIT);
        if (recBufSize == AudioRecord.ERROR ||
                recBufSize == AudioRecord.ERROR_BAD_VALUE) {
            supportRecording = false;
        }

    }
    private void updateNativeAudioUI() {
        if (!supportRecording) {
            statusView.setText(getString(R.string.mic_error_msg));
            controlButton.setEnabled(false);
            return;
        }

        statusView.setText(getString(R.string.fast_audio_info_msg,
                nativeSampleRate, nativeSampleBufSize));
    }
    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions,
                                           @NonNull int[] grantResults) {
        if (grantResults.length != 1  ||
            grantResults[0] != PackageManager.PERMISSION_GRANTED) {
            String prompt_msg = "";
            String err_msg = "";
            if (AUDIO_ECHO_REQUEST == requestCode) {
                prompt_msg = getString(R.string.permission_prompt_msg);
                err_msg = getString(R.string.permission_error_msg);
            } else if (READ_EXTERNAL_STORAGE_REQUEST == requestCode) {
                prompt_msg = "This sample needs READ_EXTERNAL_STORAGE permission";
                err_msg = "Permission for READ_EXTERNAL_STORAGE was denied";
            } else if (WRITE_EXTERNAL_STORAGE_REQUEST == requestCode) {
                prompt_msg = "This sample needs WRITE_EXTERNAL_STORAGE permission";;
                err_msg = "Permission for WRITE_EXTERNAL_STORAGE was denied";
            }
            /*
             * When user denied permission, throw a Toast to prompt that RECORD_AUDIO
             * is necessary; also display the status on UI
             * Then application goes back to the original state: it behaves as if the button
             * was not clicked. The assumption is that user will re-click the "start" button
             * (to retry), or shutdown the app in normal way.
             */
            statusView.setText(err_msg);
            Toast.makeText(getApplicationContext(),
                    prompt_msg,
                    Toast.LENGTH_SHORT).show();
            return;
        }

        /*
         * if any permission failed, the sample could not play
         */
        if (AUDIO_ECHO_REQUEST == requestCode) {
            /*
             * When permissions are granted, we prompt the user the status. User would
             * re-try the "start" button to perform the normal operation. This saves us the extra
             * logic in code for async processing of the button listener.
             */
            statusView.setText(getString(R.string.permission_granted_msg, getString(R.string.cmd_start_echo)));
            // The callback runs on app's thread, so we are safe to resume the action
            startEcho(surfaceHolder.getSurface());
        } else if (READ_EXTERNAL_STORAGE_REQUEST == requestCode) {

        } else if (WRITE_EXTERNAL_STORAGE_REQUEST == requestCode) {

        } else {
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
            return;
        }
    }

    /*
     * Loading our lib
     */
    static {
        System.loadLibrary("avutil");
        System.loadLibrary("swscale");
        System.loadLibrary("swresample");
        System.loadLibrary("avcodec");
        System.loadLibrary("avformat");
        System.loadLibrary("avfilter");
        System.loadLibrary("avdevice");

        System.loadLibrary("echo");
    }

    /*
     * jni function declarations for media player
     */
    static native long playerOpen(String url);
    static native long playerOpenAt(String url, float position);
    static native int playerPlay(long handle);
    static native int playerPause(long handle);
    static native int playerClose(long handle);
    static native int playerSeek(long handle, float position);

    static native int playerRenderVideo(long handle, Object surface);

    /*
     * jni function declarations for audio rendering
     */
    static native void createSLEngine(int rate, int framesPerBuf,
                                      long delayInMs, float decay);
    static native void deleteSLEngine();
    static native boolean configureEcho(int delayInMs, float decay);
    static native boolean createSLBufferQueueAudioPlayer();
    static native void deleteSLBufferQueueAudioPlayer();

    static native boolean createAudioRecorder();
    static native void deleteAudioRecorder();
    static native void startPlay(Object surface);
    static native void stopPlay();
}
