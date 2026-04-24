# Use Case:
- Screen capture
- Screen capture from second Wayland compositor

# Compile:
Ensure necessary packages installed (wayland-headers, libdrm, libegl, libgl)

Edit the file to select your correct GPU ("/dev/dri/card1" is the default one, as that is usually the second one)

Then run:

```gcc kmsgrab-stdout.c -o kmsgrab -ldrm -lEGL -lGL -I/usr/include/libdrm -lwayland-client```

# Running
Example to ffplay for a screen 2560x1440 that is running at the wayland display wayland-1. You do not need DEBUG=1 envvars, those are there if something breaks. Needs sudo to open the framebuffer (Wayland security):

```sudo XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-1 MESA_DEBUG=1 GBM_DEBUG=1 LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug ./kmsgrab | ffplay -fflags nobuffer -flags low_delay -probesize 20000 -analyzeduration 1 -strict experimental -f rawvideo -video_size 2560x1440 -pixel_format rgba -framerate 60 -i /dev/stdin -f rawvideo```

in the directory WIP are some drafts and variants of the script, feel free to explore. 
Entire code was indeed vibecoded with AI, did not expect it to work at the end, as the AI always used the wrong libs and APIs

For a better and more user-friendly version look at https://git.dec05eba.com/gpu-screen-recorder/about/
