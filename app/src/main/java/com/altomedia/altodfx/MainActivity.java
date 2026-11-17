package com.altomedia.altodfx;

import static android.os.Environment.DIRECTORY_DOCUMENTS;

import android.app.AlertDialog;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.media.AudioDeviceInfo;
import android.media.AudioManager;
import android.os.Build;
import android.os.Bundle;

import com.google.android.material.appbar.MaterialToolbar;
import com.google.android.material.snackbar.Snackbar;
import com.shajikhan.ladspa.amprack.AudioEngine;

import androidx.annotation.NonNull;
import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AppCompatActivity;

import android.util.Log;
import android.view.LayoutInflater;
import android.view.View;

import androidx.appcompat.app.AppCompatDelegate;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.navigation.NavController;
import androidx.navigation.Navigation;
import androidx.navigation.ui.AppBarConfiguration;
import androidx.navigation.ui.NavigationUI;
import androidx.preference.PreferenceManager;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import com.altomedia.altodfx.databinding.ActivityMainBinding;
import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import android.view.Menu;
import android.view.MenuItem;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CompoundButton;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import android.widget.ToggleButton;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStreamWriter;
import java.nio.file.Files;
import java.nio.file.Paths;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Iterator;
import java.util.List;
import java.util.Objects;
import java.util.StringJoiner;

import android.Manifest;
import android.widget.Toolbar;

public class MainActivity extends AppCompatActivity {
    public static String TAG = "MainActivity";
    private static final int AUDIO_EFFECT_REQUEST = 0;
    boolean running = false;
    JSONObject allPlugins, ampModels, effectPresets;
    ArrayList<String> presetsForAdapter ;
    private AppBarConfiguration appBarConfiguration;
    private ActivityMainBinding binding;
    ArrayList <Integer> pluginIDs = new ArrayList<>();
    public static Context context;
    public static MainActivity mainActivity;
    String [] factoryPresets;
    String presetsDir ;
    RecyclerView recyclerView;
    com.altomedia.altodfx.DataAdapter dataAdapter;
    JSONObject availablePlugins, availablePluginsLV2;
    static String[] sharedLibraries;
    static String[] sharedLibrariesLV2;
    Spinner spinner ;
    ArrayAdapter<String> adapter ;
    static boolean prepared = false ;
    static SharedPreferences defaultSharedPreferences ;
    private HeadsetReceiver headsetReceiver ;

    static {
        System.loadLibrary("amprack");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        mainActivity = this;
        context = this;


        binding = ActivityMainBinding.inflate(getLayoutInflater());
        setContentView(binding.getRoot());
//        AppCompatDelegate.setDefaultNightMode(AppCompatDelegate.MODE_NIGHT_YES);

        ampModels = loadJSONFromAssetFile(this, "amps.json");
        effectPresets = loadJSONFromAssetFile(this, "effect_presets.json");

        defaultSharedPreferences = PreferenceManager.getDefaultSharedPreferences(context);

        presetsDir = getExternalFilesDir(DIRECTORY_DOCUMENTS).getPath();
        File pD = getExternalFilesDir(DIRECTORY_DOCUMENTS);
        if (! pD.exists()) {
            if (!pD.mkdirs()) {
                Toast.makeText(context, "cannot create preset folder", Toast.LENGTH_SHORT).show();
            }
        } else {
            Log.d(TAG, String.format ("[preset dir]: %s", presetsDir));
        }

        ToggleButton preview = findViewById(R.id.preview);

        Button deletePreset = findViewById(R.id.delete_preset);
        deletePreset.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                AlertDialog.Builder builder = new AlertDialog.Builder(context);
                builder.setTitle("Delete this preset?").setMessage(spinner.getSelectedItem().toString());
                builder.setPositiveButton("Delete preset", new DialogInterface.OnClickListener() {
                    public void onClick(DialogInterface dialog, int id) {
                        deleteUserPreset();
                    }
                });

                builder.setNegativeButton("Cancel", null);
                builder.show();
            }
        });

        preview.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                if (isChecked) {
//                    buttonView.setButtonDrawable(getResources().getDrawable(R.drawable.mute));
                    AudioEngine.setOutputVolume(1f);
                    if (! running)
                        startEffect();
                    else
                        Log.i(TAG, "onCheckedChanged: effect already running");
                } else {
//                    buttonView.setButtonDrawable(getResources().getDrawable(R.drawable.preview));
                    AudioEngine.setOutputVolume(0f);

                    stopEffect();
                }
            }
        });

        ToggleButton pause = findViewById(R.id.pause);
        pause.setOnCheckedChangeListener(new CompoundButton.OnCheckedChangeListener() {
            @Override
            public void onCheckedChanged(CompoundButton buttonView, boolean isChecked) {
                buttonView.setCompoundDrawables(null, null, null, null);
                if (isChecked) {
                    buttonView.setButtonDrawable(getResources().getDrawable(R.drawable.play));
                    AudioEngine.bypass(true);
                    AudioEngine.setInputVolume(0f);
                } else {
                    buttonView.setButtonDrawable(getResources().getDrawable(R.drawable.pause));
                    AudioEngine.bypass(false);
                    AudioEngine.setInputVolume(1f);
                }

            }
        });

        setSupportActionBar(binding.toolbar);
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayShowTitleEnabled(false);
        }

        Button test = findViewById(R.id.test);
        test.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
//                Log.d(TAG, String.format ("[get preset]: %s", getPreset()));
//                loadPreset(getPreset());
                AudioEngine.printActiveChain();
            }
        });

        binding.fab.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View view) {
                Snackbar.make(view, "Replace with your own action", Snackbar.LENGTH_LONG)
                        .setAnchorView(R.id.fab)
                        .setAction("Action", null).show();
            }
        });

        recyclerView = findViewById(R.id.recycler);
        recyclerView.setLayoutManager(new LinearLayoutManager(this));
        dataAdapter = new DataAdapter(mainActivity);
        recyclerView.setAdapter(dataAdapter);

        factoryPresets = dataAdapter.getFactoryPresets();
        if (factoryPresets.length > 0)
            Log.d(TAG, String.format ("factory presets: %s", factoryPresets));

        presetsForAdapter = new ArrayList<>(Arrays.asList(factoryPresets));
        File [] userPresets = getExternalFilesDir(DIRECTORY_DOCUMENTS).listFiles();
        Log.d(TAG, String.format ("[user presets]: %d", userPresets.length));
        for (File file: userPresets) {
            presetsForAdapter.add(file.getName());
        }

        spinner = findViewById(R.id.presets);
        adapter = new ArrayAdapter<String>(mainActivity,
                android.R.layout.simple_spinner_item, presetsForAdapter);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);

        Button addPreset = findViewById(R.id.add_preset);
        addPreset.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                saveUserPreset();
            }
        });

        spinner.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override
            public void onItemSelected(AdapterView<?> parent, View view, int position, long id) {
                int selection = spinner.getSelectedItemPosition();
                if (selection < factoryPresets.length) {
                    JSONObject preset = loadJSONFromAssetFile(context, new StringBuffer("presets/").append(factoryPresets [selection]).toString());
                    Log.d(TAG, String.format("%d: %s", selection, preset));
                    recyclerView.scrollToPosition(8);
                    if (preset != null)
                        loadPreset(preset);
                    recyclerView.scrollToPosition(0);
                } else {
                    try {
                        String preset = Files.readAllLines (Paths.get (new StringJoiner ("/").add(getExternalFilesDir(DIRECTORY_DOCUMENTS).getPath()).add((CharSequence) spinner.getAdapter().getItem(selection)).toString())).get(0).toString();
                        Log.d(TAG, String.format ("[user preset] %d: %s", selection, preset));
                        dataAdapter.loadPreset(preset);
                    } catch (IOException e) {
                        Log.e(TAG, "onItemSelected: ", e);
                    }
                }
            }

            @Override
            public void onNothingSelected(AdapterView<?> parent) {

            }
        });

        AudioEngine.create();
        AudioEngine.popFunction(); // this disables the meter output
        AudioEngine.setLibraryPath(getApplicationInfo().nativeLibraryDir);
        AudioEngine.setLazyLoad(true);
        AudioEngine.pushToLockFreeBeforeOutputVolumeAaaaaargh(true);

        // Required for input streams on Android 12+: the platform checks the
        // recorder identity when a capture stream is opened.
        AudioEngine.setPackageName(getPackageName());

        AudioEngine.setOutputVolume(0f);

        availablePlugins = loadJSONFromAssetFile(this, "all_plugins.json");
        availablePluginsLV2 = loadJSONFromAssetFile(this, "lv2_plugins.json");
        ampModels = loadJSONFromAssetFile(this, "amps.json");

        sharedLibraries = context.getResources().getStringArray(R.array.ladspa_plugins);
        sharedLibrariesLV2 = context.getResources().getStringArray(R.array.lv2_plugins);

        AudioEngine.setMainActivityClassName("com/altomedia/altodfx/MainActivity");

        loadPlugins();
        applySettings();
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
            startActivity(new Intent(context, SettingsActivity.class));
            return true;
        }

        return super.onOptionsItemSelected(item);
    }

    public static void toast(String text) {
        Toast.makeText(context, text, Toast.LENGTH_LONG).show();
    }

    private void startEffect() {
        Log.d(TAG, "Attempting to start");
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        if (!isRecordPermissionGranted()) {
            requestRecordPermission();
            return;
        }

        running = AudioEngine.setEffectOn(true);
    }

    private void stopEffect() {
        if (!running) return;

        Log.d(TAG, "Playing, attempting to stop, state: " + running);
        getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        running = ! AudioEngine.setEffectOn(false);
    }

    private boolean isRecordPermissionGranted() {
        return (ActivityCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) ==
                PackageManager.PERMISSION_GRANTED);
    }

    private void requestRecordPermission() {
        ActivityCompat.requestPermissions(
                this,
                new String[]{Manifest.permission.RECORD_AUDIO},
                AUDIO_EFFECT_REQUEST);
    }

    static public JSONObject loadJSONFromAssetFile(Context context, String filename) {
        String json = null;
        try {
            InputStream is = context.getAssets().open(filename);
            int size = is.available();
            byte[] buffer = new byte[size];
            is.read(buffer);
            is.close();
            json = new String(buffer, "UTF-8");
        } catch (IOException ex) {
            ex.printStackTrace();
            Log.e(TAG, "loadJSONFromAsset: unable to parse json " + filename, ex);
            return null;
        }

        JSONObject jsonObject = null ;
        try {
            jsonObject = new JSONObject(json);
        } catch (JSONException e) {
            e.printStackTrace();
            Log.e(TAG, "loadJSONFromAsset: cannot parse json " + filename, e);
        }

        return jsonObject;
    }

    public void addPluginToRack(int pluginID) {
        int library = pluginID / 100;
        int plug = pluginID - (library * 100);
        Log.d(TAG, "addPluginToRack: loading from " + sharedLibraries.length + " LADSPA and " + sharedLibrariesLV2.length + " LV2 plugins");
        Log.d(TAG, "Adding plugin: " + library + ": " + plug);
        int ret = -1;

        if (library > 149 /* because we have 149 lADSPA libraries */) {
            ret = AudioEngine.addPluginLazyLV2(sharedLibrariesLV2[library - sharedLibraries.length], plug);
        } else
            ret = AudioEngine.addPluginLazy(sharedLibraries[library], plug);

        dataAdapter.addItem(pluginID, ret);
    }

    void printPlugins () {
        JSONObject plugins = mainActivity.availablePluginsLV2 ;
        Iterator<String> keys = plugins.keys();

        while (keys.hasNext()) {
            String key = keys.next();
            try {

                if (plugins.get(key) instanceof JSONObject) {
                    JSONObject object = plugins.getJSONObject(key);
                    String name = object.getString("name");
                    String id = object.getString("id");
                    Log.d(TAG, String.format ("[LV2 plugin] %s: %s", id, name));
//                    mainActivity.pluginDialogAdapter.addItem(Integer.parseInt(key), name, Integer.parseInt(id));
                }
            } catch (JSONException e) {
                e.printStackTrace();
            }
        }

        plugins = mainActivity.availablePlugins ;
        keys = plugins.keys();

        while (keys.hasNext()) {
            String key = keys.next();
            try {
                if (plugins.get(key) instanceof JSONObject) {
                    JSONObject object = plugins.getJSONObject(key);
                    String name = object.getString("name");
                    String id = object.getString("id");
                    Log.d(TAG, "[LADSPA plugin]: " + name + ": " + id);
//                    mainActivity.pluginDialogAdapter.addItem(Integer.parseInt(key), name, Integer.parseInt(id));
                }
            } catch (JSONException e) {
                e.printStackTrace();
            }
        }
    }

    public static String getLV2Info (String libraryName, String plugin) {
        String pluginName ;
        if (plugin.indexOf("#") != -1)
            pluginName = plugin.split("#")[1];
        else {
            String [] p = plugin.split("/");
            pluginName = p [p.length -1];
        }

        Log.d(TAG, "getLV2Info: lv2/" + libraryName + "/" + pluginName + ".json");
        JSONObject jsonObject = loadJSONFromAssetFile(context, "lv2/" + libraryName + "/" + pluginName + ".json");
        return jsonObject.toString();
    }

    public void loadPlugins () {
        pluginIDs.clear();
        allPlugins = loadJSONFromAssetFile(context, "voice.json");
        Iterator<String> keys = allPlugins.keys();

        int index = 0;
        while (keys.hasNext()) {
            String key = keys.next();
            try {
                if (allPlugins.get(key) instanceof JSONObject) {
                    JSONObject object = allPlugins.getJSONObject(key);
                    String name = object.getString("name");
                    addPluginToRack (Integer.parseInt(key));
                    pluginIDs.add(Integer.parseInt(key));

//                    AudioEngine.togglePlugin(index, false);
                }
            } catch (JSONException e) {
                e.printStackTrace();
            }

            index ++ ;
        }
    }

    public void saveUserPreset () {
        AlertDialog.Builder builder = new AlertDialog.Builder(this);
        LayoutInflater inflater = getLayoutInflater();
        LinearLayout linearLayout = (LinearLayout) inflater.inflate(R.layout.get_filename, null);
        TextView textView = linearLayout.findViewById(R.id.filename);
        builder.setView(linearLayout)
                .setPositiveButton("Save", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dialog, int id) {
                        CharSequence filename = textView.getText() ;
                        if (filename.equals("") || filename == null)
                            return;

                        String preset = getPreset();
                        String fullFilename = new StringJoiner ("/").add(presetsDir).add(filename).toString();
                        Log.d(TAG, String.format ("write preset: %s", fullFilename));
                        if (! Objects.requireNonNull(getExternalFilesDir(DIRECTORY_DOCUMENTS)).exists())
                            Log.e(TAG, "onClick: presets directory does not exist!" );
                        if (writeFile(fullFilename, preset)) {
                            Toast.makeText(MainActivity.this, "Saved preset to file: " + filename, Toast.LENGTH_SHORT).show();
                            addToSpinner(String.valueOf(filename));
                        }
                    }
                })
                .setNegativeButton("Cancel", null);

        builder.show();
    }

    public boolean writeFile (String filename, String preset) {
        File myFile = new File(filename);

        try {
            myFile.createNewFile();
            FileOutputStream fOut = new FileOutputStream(myFile);
            OutputStreamWriter myOSW = new OutputStreamWriter(fOut);
            myOSW.append(preset);
            myOSW.close();
            fOut.close();
            return true;
        } catch (IOException e) {
            Log.e(TAG, "writeFile: ", e);
            Toast.makeText(this, "cannot create preset " + e.getMessage(), Toast.LENGTH_SHORT).show();
            return false;
        }
    }

    public void addToSpinner (String name) {
        presetsForAdapter.add(name);
        adapter = new ArrayAdapter<String>(mainActivity,
                android.R.layout.simple_spinner_item, presetsForAdapter);
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);

        spinner.setSelection(adapter.getCount() - 1);
    }

    public void deleteUserPreset () {
        if (spinner.getSelectedItemPosition() < factoryPresets.length) {
            Toast.makeText(context, "Factory preset cannot be deleted", Toast.LENGTH_SHORT).show();
            return;
        }

        String preset = spinner.getSelectedItem().toString();
        File file = new File(new StringJoiner("/").add (getExternalFilesDir(DIRECTORY_DOCUMENTS).getPath()).add("/").add (preset).toString());
        Log.d(TAG, String.format ("[delete]: %s", file.getAbsolutePath()));
        if (!file.delete())
            Toast.makeText(context, "Cannot delete preset: " + preset, Toast.LENGTH_SHORT).show();
        else {
            Toast.makeText(context, "Preset deleted", Toast.LENGTH_SHORT).show();
            presetsForAdapter.remove(preset);
            adapter = new ArrayAdapter<String>(mainActivity,
                    android.R.layout.simple_spinner_item, presetsForAdapter);
            adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
            spinner.setAdapter(adapter);

            spinner.setSelection(0);
        }

    }

    @Override
    protected void onResume() {
        super.onResume();

        headsetReceiver = new HeadsetReceiver();
        IntentFilter filter = new IntentFilter();
        filter.addAction(AudioManager.ACTION_HEADSET_PLUG);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(headsetReceiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(headsetReceiver, filter);
        }
    }

    @Override
    protected void onDestroy() {
        if (headsetReceiver != null) {
            unregisterReceiver(headsetReceiver);
            headsetReceiver = null;
        }
        super.onDestroy();
    }

    // Re-applies the headset routing whenever the headphone jack is plugged or
    // unplugged so audio always follows the 3.5 mm hole.
    private static class HeadsetReceiver extends BroadcastReceiver {
        @Override
        public void onReceive(Context context, Intent intent) {
            if (AudioManager.ACTION_HEADSET_PLUG.equals(intent.getAction())) {
                applySettings();
            }
        }
    }

    // Audio is routed through the wired headset jack (3.5 mm) by default and is
    // never routed through a USB audio device, so the app runs off the headset
    // hole only.
    public static void applySettings () {
        String input = defaultSharedPreferences.getString("input", "auto");
        String output = defaultSharedPreferences.getString("output", "auto");
        Log.d(TAG, String.format("[devices] input preference: %s, output preference: %s", input, output));

        try {
            int inputDevice = resolveInputDevice(input);
            int outputDevice = resolveOutputDevice(output);
            // A device id of 0 is valid, so "not found" is signalled with -1 and
            // left unspecified so Oboe uses the system default device.
            AudioEngine.setRecordingDeviceId(inputDevice);
            AudioEngine.setPlaybackDeviceId(outputDevice);
            // A wired headset input (iRig in the 3.5 mm jack) needs
            // InputPreset::VoiceRecognition on many phones to open the TRRS mic.
            AudioEngine.setHeadsetInput(isHeadsetInputDevice(inputDevice));
            applyInputConditioner();
        } catch (NumberFormatException e) {
            Log.e(TAG, "applySettings: ", e);
        }
    }

    // Returns true when the given input device id belongs to a wired or USB
    // headset-style interface, which needs the voice-recognition preset to open
    // its microphone on many devices.
    private static boolean isHeadsetInputDevice (int deviceId) {
        if (deviceId < 0) {
            return false;
        }
        AudioManager manager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (manager == null) {
            return false;
        }
        for (AudioDeviceInfo device : manager.getDevices(AudioManager.GET_DEVICES_INPUTS)) {
            if (device.getId() == deviceId) {
                int type = device.getType();
                return type == AudioDeviceInfo.TYPE_WIRED_HEADSET
                        || type == AudioDeviceInfo.TYPE_WIRED_HEADPHONES
                        || type == AudioDeviceInfo.TYPE_USB_HEADSET
                        || type == AudioDeviceInfo.TYPE_USB_DEVICE
                        || type == AudioDeviceInfo.TYPE_USB_ACCESSORY;
            }
        }
        return false;
    }

    // The front-end conditioner removes DC offset, mains hum and hiss before the
    // signal reaches the effect chain. Without it a guitar into a headset-jack
    // interface tends to hum and buzz.
    public static void applyInputConditioner () {
        boolean enabled = defaultSharedPreferences.getBoolean("input_clean", true);
        float gate = Float.parseFloat(defaultSharedPreferences.getString("noise_gate", "0.0035"));
        float highpass = Float.parseFloat(defaultSharedPreferences.getString("input_highpass", "80"));
        AudioEngine.setInputConditionerEnabled(enabled);
        AudioEngine.setNoiseGateThreshold(gate);
        AudioEngine.setInputHighpass(highpass);
        Log.d(TAG, String.format("[conditioner] enabled: %s, gate: %f, highpass: %f",
                enabled, gate, highpass));
    }

    public static void applyInputDevice (String preference) {
        AudioEngine.setRecordingDeviceId(resolveInputDevice(preference));
        AudioEngine.setHeadsetInput(isHeadsetInputDevice(resolveInputDevice(preference)));
    }

    public static void applyOutputDevice (String preference) {
        AudioEngine.setPlaybackDeviceId(resolveOutputDevice(preference));
    }

    private static boolean isUsbAudioDevice (AudioDeviceInfo device) {
        int type = device.getType();
        return type == AudioDeviceInfo.TYPE_USB_DEVICE
                || type == AudioDeviceInfo.TYPE_USB_ACCESSORY
                || type == AudioDeviceInfo.TYPE_USB_HEADSET;
    }

    private static AudioDeviceInfo findDevice (AudioManager manager, boolean input, int [] types) {
        AudioDeviceInfo[] devices = manager.getDevices(
                input ? AudioManager.GET_DEVICES_INPUTS : AudioManager.GET_DEVICES_OUTPUTS);

        for (int type : types) {
            for (AudioDeviceInfo device : devices) {
                if (device.getType() == type && ! isUsbAudioDevice(device)) {
                    return device;
                }
            }
        }

        return null;
    }

    private static int resolveInputDevice (String preference) {
        AudioManager manager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (manager == null) {
            return -1;
        }

        if ("phone".equals(preference)) {
            AudioDeviceInfo device = findDevice(manager, true, new int [] {AudioDeviceInfo.TYPE_BUILTIN_MIC});
            return device != null ? device.getId() : -1;
        }

        AudioDeviceInfo headset = findDevice(manager, true, new int [] {
                AudioDeviceInfo.TYPE_WIRED_HEADSET,
                AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
                AudioDeviceInfo.TYPE_AUX_LINE
        });

        if (headset != null) {
            return headset.getId();
        }

        if ("headset".equals(preference)) {
            Toast.makeText(context, "No headset detected. Connect a wired headset to the headphone jack.", Toast.LENGTH_LONG).show();
            return -1;
        }

        // Auto: fall back to the built-in microphone, never a USB device.
        AudioDeviceInfo builtIn = findDevice(manager, true, new int [] {AudioDeviceInfo.TYPE_BUILTIN_MIC});
        return builtIn != null ? builtIn.getId() : -1;
    }

    private static int resolveOutputDevice (String preference) {
        AudioManager manager = (AudioManager) context.getSystemService(Context.AUDIO_SERVICE);
        if (manager == null) {
            return -1;
        }

        if ("phone".equals(preference)) {
            AudioDeviceInfo device = findDevice(manager, false, new int [] {AudioDeviceInfo.TYPE_BUILTIN_SPEAKER});
            return device != null ? device.getId() : -1;
        }

        AudioDeviceInfo headset = findDevice(manager, false, new int [] {
                AudioDeviceInfo.TYPE_WIRED_HEADPHONES,
                AudioDeviceInfo.TYPE_WIRED_HEADSET,
                AudioDeviceInfo.TYPE_AUX_LINE
        });

        if (headset != null) {
            return headset.getId();
        }

        if ("headset".equals(preference)) {
            Toast.makeText(context, "No headset detected. Connect headphones to the headphone jack.", Toast.LENGTH_LONG).show();
            return -1;
        }

        // Auto: fall back to the built-in speaker, never a USB device.
        AudioDeviceInfo builtIn = findDevice(manager, false, new int [] {AudioDeviceInfo.TYPE_BUILTIN_SPEAKER});
        return builtIn != null ? builtIn.getId() : -1;
    }

    public static String getPreset1 () {
        JSONObject jsonObject = loadJSONFromAssetFile(context, "voice.json");
        Iterator<String> keys = jsonObject.keys();

        JSONObject preset = new JSONObject();
        int counter = 0 ;

        try {
            while (keys.hasNext()) {
                String key = keys.next();
                JSONObject plugin = jsonObject.getJSONObject(key);
                Log.d(TAG, String.format ("[plugin]: %s", plugin));
                JSONObject controls = plugin.getJSONObject("controls");

                JSONObject pluginControls = new JSONObject();
                Iterator <String> control = controls.keys();

                int cnt = 0 ;
                while (control.hasNext()) {
                    String c = control.next();
                    JSONObject cont = controls.getJSONObject(c);
                    Log.i(TAG, "getPreset: " + cont);

                    if (cont.get("index") instanceof Integer)
                        pluginControls.put(String.valueOf(cnt++), AudioEngine.getActivePluginValueByIndex(counter, cont.getInt("index")));
                    else {
                        JSONArray jsonArray = cont.getJSONArray("index");
                        for (int index = 0 ; index < jsonArray.length(); index ++) {
                            pluginControls.put(String.valueOf(cnt++), AudioEngine.getActivePluginValueByIndex(counter, index));
                        }
                    }
                }

                preset.put(String.valueOf(counter), pluginControls);
                counter ++ ;
            }
        } catch (JSONException jsonException) {
            Log.e(TAG, "getPreset: ", jsonException);
            Toast.makeText(context, jsonException.getMessage(), Toast.LENGTH_SHORT).show();
        }

        Log.d(TAG, String.format ("[preset]: %s", preset.toString()));
        return preset.toString();
    }

    public static String getPreset () {
        int plugins = AudioEngine.getActivePlugins();
        JSONObject preset = new JSONObject();

        try {
            for (int i = 0; i < plugins; i++) {
                JSONObject plugin = new JSONObject();
                plugin.put("active", AudioEngine.getActivePluginEnabled(i));
                float [] values = AudioEngine.getActivePluginValues(i);
                JSONArray arrayList = new JSONArray(values);

                plugin.put("controls", arrayList);
                plugin.put("name", AudioEngine.getActivePluginName(i));
                preset.put(String.valueOf(i), plugin);
            }
        } catch (JSONException e) {
            Toast.makeText(context, e.getMessage(), Toast.LENGTH_SHORT).show();
            Log.e(TAG, "getPreset: ", e);
            return null;
        }

        Log.d(TAG, "getPreset: generated: " + preset);
        return preset.toString();
    }

    public void loadPreset (JSONObject jsonObject) {
        dataAdapter.clear();
//        Log.d(TAG, "loadPreset: default: " + getPreset());
        Log.d(TAG, String.format ("[loading plugins from preset (%d)]: %s", jsonObject.length(), jsonObject));
        try {
            for (int key = 0 ; key < jsonObject.length() ; key ++) {
                JSONObject object = jsonObject.getJSONObject(String.valueOf(key));
                JSONArray con = object.getJSONArray("controls");

                Log.d(TAG, String.format ("[%d: %s]: %s", key, object.getString("name"), con));
                AudioEngine.togglePlugin(key, object.getBoolean("active"));
                for (int a = 0 ; a < con.length() ; a++) {
                    AudioEngine.setPluginControl(key, a, (float) con.getDouble(a));
                    Log.i(TAG, "loadPreset: " + String.format ("%d %d: %f", key, a, con.getDouble(a)));
                }

                Log.d(TAG, String.format ("[addItem] %d: %d", pluginIDs.get(key), key));
                dataAdapter.addItem(pluginIDs.get(key), key);
            }
        } catch (JSONException e) {
            Log.e(TAG, "loadPreset: ", e);
            return;
        }

//        loadPlugins();
    }

    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
                == PackageManager.PERMISSION_GRANTED) {
            startEffect();
        }
    }
}

