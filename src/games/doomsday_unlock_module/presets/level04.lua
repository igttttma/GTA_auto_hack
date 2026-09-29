-- 预设脚本：每个调用是一步，可单步执行
-- rotate 负责定位（自动选中，到位自停），wait(destroyed) 确认战果
-- 谓词: beam.hits(id) / beam.terminal(id) / destroyed(id) / selected(id)
--       any(p1, p2) / all(p1, p2) 组合条件
-- 查询: angles() / alive()
rotate("M01", 120)
rotate("M02", 110)
wait(destroyed("T02"), { timeout = 10 })
rotate("M01", 180)
rotate("M09", 110)
wait(destroyed("T04"), { timeout = 10 }) 
rotate("M02", 180)
rotate("M08", 95)
wait(destroyed("T08"), { timeout = 10 }) 
rotate("M09", 135)
rotate("M06", 50)
wait(destroyed("T07"), { timeout = 10 }) 
rotate("M09", 180)
rotate("M07", 8)
wait(destroyed("T03"), { timeout = 10 }) 
wait(destroyed("T05"), { timeout = 10 }) 
rotate("M07", 35)
rotate("M04", 5)
wait(destroyed("T09"), { timeout = 10 }) 
rotate("M06", 70)
rotate("M03", 65)
wait(destroyed("T06"), { timeout = 10 }) 
rotate("M08", 135)



