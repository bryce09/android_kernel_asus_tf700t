cmd_kernel/power/built-in.o :=  /opt/toolchains/androideabi-4.6/bin/ld -EL    -r -o kernel/power/built-in.o kernel/power/main.o kernel/power/console.o kernel/power/process.o kernel/power/suspend.o kernel/power/suspend_expire.o kernel/power/wakelock.o kernel/power/userwakelock.o kernel/power/earlysuspend.o kernel/power/fbearlysuspend.o kernel/power/suspend_time.o kernel/power/poweroff.o 
