break using.rig:26
break using.rig:33
run
print p
print p.entity.position.x
python
try:
    gdb.execute("print p.position")
except gdb.error as e:
    print(e)
end
print s.transform.x
continue
print e.position
print *e
print position
