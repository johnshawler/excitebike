# injected into run.sh: start the game, hold accelerate, take screenshots with the speed overlay (game frame / real 50 Hz tick / skipped frames)
xdotool key Return; sleep 3
xdotool key Return; sleep 8
full a1
xdotool keydown Control_R; sleep 12
full a2
sleep 12
full a3
xdotool keydown Up; sleep 0.5; xdotool keyup Up
sleep 10
full a4
