package com.altomedia.altodfx;

import android.os.Bundle;

import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AppCompatActivity;
import androidx.preference.ListPreference;
import androidx.preference.Preference;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.PreferenceManager;
import androidx.preference.SwitchPreferenceCompat;

import com.shajikhan.ladspa.amprack.AudioEngine;

import java.util.StringJoiner;

public class SettingsActivity extends AppCompatActivity {

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.settings_activity);
        if (savedInstanceState == null) {
            getSupportFragmentManager()
                    .beginTransaction()
                    .replace(R.id.settings, new SettingsFragment())
                    .commit();
        }
        ActionBar actionBar = getSupportActionBar();
        if (actionBar != null) {
            actionBar.setDisplayHomeAsUpEnabled(true);
        }
    }

    public static class SettingsFragment extends PreferenceFragmentCompat {

        @Override
        public void onCreatePreferences(Bundle savedInstanceState, String rootKey) {
            setPreferencesFromResource(R.xml.root_preferences, rootKey);

            ListPreference listPreference = findPreference("input");
            ListPreference listPreferenceOutput = findPreference("output");
            ListPreference noiseGate = findPreference("noise_gate");
            ListPreference inputHighpass = findPreference("input_highpass");
            SwitchPreferenceCompat inputClean = findPreference("input_clean");

            inputClean.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    MainActivity.applyInputConditioner();
                    return true;
                }
            });

            noiseGate.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    MainActivity.applyInputConditioner();
                    return true;
                }
            });

            inputHighpass.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    MainActivity.applyInputConditioner();
                    return true;
                }
            });

            listPreference.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    MainActivity.applyInputDevice(newValue.toString());
                    return true;
                }
            });

            listPreferenceOutput.setOnPreferenceChangeListener(new Preference.OnPreferenceChangeListener() {
                @Override
                public boolean onPreferenceChange(Preference preference, Object newValue) {
                    MainActivity.applyOutputDevice(newValue.toString());
                    return true;
                }
            });

            Preference about = findPreference("about");
            about.setSummary(
                    new StringJoiner(" ")
                            .add("Version")
                            .add(BuildConfig.VERSION_NAME)
                            .add("Build")
                            .add(String.valueOf(BuildConfig.VERSION_CODE))
                            .toString()
            );
        }
    }
}
