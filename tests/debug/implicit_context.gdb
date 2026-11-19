break implicit_context.rig:21
break implicit_context.rig:23
run
print context == &default_context
print context->allocator.proc
continue
print context == &other
print twice(context, 3)
ptype twice
break twice
continue
info args
set $inner = context
up
print $inner == &other
delete
break implicit_context.rig:14
break implicit_context.rig:16
continue
info locals
continue
print context == &default_context
next
print context
