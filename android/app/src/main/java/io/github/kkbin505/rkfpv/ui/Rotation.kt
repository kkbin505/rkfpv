package io.github.kkbin505.rkfpv.ui

import android.app.Activity
import android.content.pm.ActivityInfo
import android.provider.Settings
import android.view.OrientationEventListener

/**
 * Full screen preview the way video apps do it: turning the phone sideways
 * goes full screen by itself (MainActivity watches the configuration); the
 * button and Back force it.
 *
 * Forcing locks the orientation, then hands it back to the sensor once the
 * phone is physically held that way, so turning it afterwards works as
 * usual: tap full screen, turn the phone sideways, turn it back upright, and
 * full screen ends. With auto-rotate off the lock stays until it is left
 * with the button or Back.
 */
class Rotation(private val activity: Activity) {
    private enum class Want { LANDSCAPE, PORTRAIT }

    private var want: Want? = null

    private val listener = object : OrientationEventListener(activity) {
        override fun onOrientationChanged(deg: Int) {
            if (deg == ORIENTATION_UNKNOWN) return
            val held = when {
                deg in 60..120 || deg in 240..300 -> Want.LANDSCAPE
                deg <= 30 || deg >= 330 -> Want.PORTRAIT
                else -> return
            }
            if (held == want) follow()
        }
    }

    fun enterFullscreen() {
        activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        waitFor(Want.LANDSCAPE)
    }

    fun exitFullscreen() {
        if (!autoRotate()) {
            // rotation locked: "follow" is the user's portrait
            follow()
            return
        }
        activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        waitFor(Want.PORTRAIT)
    }

    fun release() {
        listener.disable()
    }

    private fun waitFor(w: Want) {
        if (!autoRotate()) {
            want = null
            listener.disable()
            return
        }
        want = w
        if (listener.canDetectOrientation()) listener.enable() else follow()
    }

    /** back to the sensor and the user's auto-rotate setting */
    private fun follow() {
        want = null
        listener.disable()
        activity.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
    }

    private fun autoRotate(): Boolean =
        Settings.System.getInt(activity.contentResolver, Settings.System.ACCELEROMETER_ROTATION, 0) == 1
}
