ptype 'ring(N = 2, T = int)'
break 'pick(T = Vec2)'
run
bt 1
print *a
continue
info breakpoints
delete
break generics.rig:20
run
bt 1
info args
continue
bt 1
continue
delete
rbreak ^pick(
run
bt 1
continue
bt 1
continue
break pick
