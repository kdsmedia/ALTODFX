package com.altomedia.altodfx;

import android.os.Bundle;

import androidx.appcompat.app.ActionBar;
import androidx.appcompat.app.AppCompatActivity;
import androidx.preference.ListPreference;
import androidx.preference.Preference;
import androidx.preference.PreferenceFragmentCompat;
import androidx.preference.PreferenceManager;

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

            Preference lamePreset = findPreference("lame_preset");
            lamePreset.setOnPreferenceClickListener(new Preference.OnPreferenceClickListener() {
                @Override
                public boolean onPreferenceClick(Preference preference) {
                    AudioEngine.setLamePreset(Integer.parseInt(
                            PreferenceManager.getDefaultSharedPreferences(requireContext())
                                    .getString("lame_preset", "1001")));
                    return false;
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
