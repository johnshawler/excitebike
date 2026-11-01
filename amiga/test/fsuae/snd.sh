hold() { xdotool keydown $1; sleep ${2:-0.6}; xdotool keyup $1; }
sleep 10
hold Return; sleep 8
hold Return; sleep 8
hold Return; sleep 10
xdotool keydown z; sleep 25
