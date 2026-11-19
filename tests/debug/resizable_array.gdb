break __array_add
run
bt 2
delete
break resizable_array.rig:14
continue
print numbers.count
print numbers.allocated
print *numbers.data@numbers.count
print numbers.allocator.proc
ptype numbers
print points.data[0]
break resizable_array.rig:25
continue
print a
print a->count
