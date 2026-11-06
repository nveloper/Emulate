# The native frontend looks NativeBridge up by name and calls its static callbacks
# (onVideoSize, onMessage, onNetplayStatus, onRumble) from C++.
-keep class com.emulate.app.NativeBridge { *; }
