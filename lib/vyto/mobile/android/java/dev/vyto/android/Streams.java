package dev.vyto.android;

import android.content.Context;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.HandlerThread;

/**
 * Java half of {@code sensors.vt} and {@code location.vt}: everything that
 * arrives as a stream of samples rather than as a one-shot result.
 *
 * <p>One class for both because they share the machinery that matters — a
 * {@link HandlerThread} to receive on, and one push into the native slot table
 * ({@code native/src/astream.c}). Sensors and location are also the same thing
 * from the app's side: state that updates on its own, which Vyto polls.
 *
 * <p><b>Never the UI thread.</b> Both listeners are registered against this
 * class's own Looper. An accelerometer at {@code SENSOR_DELAY_GAME} delivers
 * 50-200 samples a second, and running that through the UI thread would fight
 * the very frames it exists to inform.
 *
 * <p><b>Nothing is queued.</b> {@link Native#streamPut} overwrites the channel's
 * slot. A sensor is state, not a series of events: the reader wants the current
 * value, and a queue between reads would grow without bound for no gain.
 *
 * <p>The Activity owns the lifecycle: it constructs this, binds it, and calls
 * {@link #stopAll} in {@code onPause}. A sensor left registered while the app
 * is backgrounded is a battery bug and nothing else will catch it.
 *
 * <p>STATUS: written, never run on a device.
 */
public final class Streams implements SensorEventListener, LocationListener {

    // Channel ids. Must match sensors.vt / location.vt, and the slot count in
    // astream.c (VTA_STREAM_CHANNELS).
    public static final int CH_LOCATION = 0;
    public static final int CH_ACCEL    = 1;
    public static final int CH_GYRO     = 2;
    public static final int CH_MAG      = 3;
    public static final int CH_LIGHT    = 4;
    public static final int CH_PRESSURE = 5;
    public static final int CH_STEPS    = 6;

    private final Context context;
    private final SensorManager sensors;
    private final LocationManager locations;
    private final HandlerThread thread;
    private final Handler handler;
    private boolean locating;

    public Streams(Context c) {
        this.context = c.getApplicationContext();
        this.sensors = (SensorManager) context.getSystemService(Context.SENSOR_SERVICE);
        this.locations = (LocationManager) context.getSystemService(Context.LOCATION_SERVICE);
        this.thread = new HandlerThread("vyto-streams");
        this.thread.start();
        this.handler = new Handler(thread.getLooper());
    }

    /** Stop everything and drop the thread. Called from {@code onDestroy}. */
    public void dispose() {
        stopAll();
        thread.quitSafely();
    }

    /** Unregister every live stream, keeping the thread. For {@code onPause}. */
    public void stopAll() {
        if (sensors != null) sensors.unregisterListener(this);
        stopLocation();
    }

    // ------------------------------------------------------------- sensors

    private static int sensorTypeOf(int channel) {
        switch (channel) {
            case CH_ACCEL:    return Sensor.TYPE_ACCELEROMETER;
            case CH_GYRO:     return Sensor.TYPE_GYROSCOPE;
            case CH_MAG:      return Sensor.TYPE_MAGNETIC_FIELD;
            case CH_LIGHT:    return Sensor.TYPE_LIGHT;
            case CH_PRESSURE: return Sensor.TYPE_PRESSURE;
            case CH_STEPS:    return Sensor.TYPE_STEP_COUNTER;
            default:          return -1;
        }
    }

    private static int channelOf(int sensorType) {
        switch (sensorType) {
            case Sensor.TYPE_ACCELEROMETER: return CH_ACCEL;
            case Sensor.TYPE_GYROSCOPE:     return CH_GYRO;
            case Sensor.TYPE_MAGNETIC_FIELD:return CH_MAG;
            case Sensor.TYPE_LIGHT:         return CH_LIGHT;
            case Sensor.TYPE_PRESSURE:      return CH_PRESSURE;
            case Sensor.TYPE_STEP_COUNTER:  return CH_STEPS;
            default:                        return -1;
        }
    }

    /** True when the device actually has this sensor. Many do not. */
    public boolean hasSensor(int channel) {
        int type = sensorTypeOf(channel);
        return sensors != null && type >= 0 && sensors.getDefaultSensor(type) != null;
    }

    /**
     * Start delivering samples on {@code channel}. {@code rateUs} is the
     * sampling period in microseconds, as {@code SensorManager} means it — a
     * hint, not a contract; the platform delivers no faster than it wants to.
     */
    public boolean startSensor(int channel, int rateUs) {
        int type = sensorTypeOf(channel);
        if (sensors == null || type < 0) return false;
        Sensor s = sensors.getDefaultSensor(type);
        if (s == null) return false;
        return sensors.registerListener(this, s, rateUs, handler);
    }

    public void stopSensor(int channel) {
        int type = sensorTypeOf(channel);
        if (sensors == null || type < 0) return;
        Sensor s = sensors.getDefaultSensor(type);
        if (s != null) sensors.unregisterListener(this, s);
    }

    @Override
    public void onSensorChanged(SensorEvent e) {
        if (!Native.isLoaded()) return;
        int ch = channelOf(e.sensor.getType());
        if (ch < 0) return;
        float[] v = e.values;
        double a = v.length > 0 ? v[0] : 0.0;
        double b = v.length > 1 ? v[1] : 0.0;
        double c = v.length > 2 ? v[2] : 0.0;
        // e.timestamp is nanoseconds since boot, not epoch: it is only ever
        // compared against itself, and Vyto treats a non-zero value as "a
        // sample exists".
        Native.streamPut(ch, a, b, c, 0.0, 0.0, e.timestamp / 1000000L);
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) { /* not surfaced */ }

    // ------------------------------------------------------------ location

    /**
     * Start location updates. Returns false when the permission has not been
     * granted or no provider is enabled — both of which are normal states, not
     * errors: the caller asks for the permission through {@code Actions} and
     * tries again.
     */
    public boolean startLocation(long minMs, double minM) {
        if (locations == null) return false;
        if (context.checkSelfPermission(android.Manifest.permission.ACCESS_FINE_LOCATION)
                != android.content.pm.PackageManager.PERMISSION_GRANTED
            && context.checkSelfPermission(android.Manifest.permission.ACCESS_COARSE_LOCATION)
                != android.content.pm.PackageManager.PERMISSION_GRANTED) {
            return false;
        }
        String provider = LocationManager.GPS_PROVIDER;
        if (!locations.isProviderEnabled(provider)) provider = LocationManager.NETWORK_PROVIDER;
        if (!locations.isProviderEnabled(provider)) return false;
        try {
            locations.requestLocationUpdates(provider, minMs, (float) minM, this,
                                             thread.getLooper());
            locating = true;
            // The last known fix immediately, so a screen that opens a map does
            // not sit blank for the first update interval.
            Location last = locations.getLastKnownLocation(provider);
            if (last != null) push(last);
            return true;
        } catch (SecurityException e) {
            android.util.Log.w("Vyto", "location denied", e);
            return false;
        }
    }

    public void stopLocation() {
        if (locations == null || !locating) return;
        locating = false;
        try { locations.removeUpdates(this); }
        catch (SecurityException e) { /* already gone */ }
        // Clear the slot: a stopped stream must not keep handing back an old
        // fix, or poll() reports a position the app is no longer tracking.
        if (Native.isLoaded()) Native.streamPut(CH_LOCATION, 0, 0, 0, 0, 0, 0);
    }

    private void push(Location l) {
        if (!Native.isLoaded()) return;
        Native.streamPut(CH_LOCATION, l.getLatitude(), l.getLongitude(),
                         l.hasAltitude() ? l.getAltitude() : 0.0,
                         l.hasSpeed() ? l.getSpeed() : 0.0,
                         l.hasAccuracy() ? l.getAccuracy() : 0.0,
                         l.getTime());
    }

    @Override
    public void onLocationChanged(Location location) { push(location); }

    // Required by LocationListener below API 30; harmless above.
    @Override public void onStatusChanged(String provider, int status, Bundle extras) { }
    @Override public void onProviderEnabled(String provider) { }
    @Override public void onProviderDisabled(String provider) { }
}
