/*
 * Copyright (C) 2012 The Android Open Source Project
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

package com.android.bluetooth.opp;

import android.bluetooth.BluetoothDevice;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.util.Log;

public class BluetoothOppHandoverReceiver extends BroadcastReceiver {
    public static final String TAG ="BluetoothOppHandoverReceiver";
    private static final boolean D = Constants.DEBUG;
    private static final boolean V = Constants.VERBOSE;

    @Override
    public void onReceive(Context context, Intent intent) {
        String action = intent.getAction();
        if (action.equals(Constants.ACTION_WHITELIST_DEVICE)) {
            BluetoothDevice device =
                    (BluetoothDevice)intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE);
            if (D) Log.d(TAG, "Adding " + device + " to whitelist");
            if (device == null) return;
            BluetoothOppManager.getInstance(context).addToWhitelist(device.getAddress());
        } else {
            if (D) Log.d(TAG, "Unknown action: " + action);
        }
    }

}
