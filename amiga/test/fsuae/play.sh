hold() { xdotool keydown $1; sleep ${2:-0.6}; xdotool keyup $1; }
hold Return; sleep 8
hold Return; sleep 8
hold Return; sleep 15
xdotool keydown z
full a1
sleep 8
full a2
sleep 12
full a3
