-- 预设脚本：每个调用是一步，可单步执行
-- rotate 负责定位（自动选中，到位自停），wait(destroyed) 确认战果
-- 谓词: beam.hits(id) / beam.terminal(id) / destroyed(id) / selected(id)
--       any(p1, p2) / all(p1, p2) 组合条件
-- 查询: angles() / alive()
rotate("M09", 5)
rotate("M02", 165)
wait(destroyed("T03"), { timeout = 10 })
rotate("M09", 175)
rotate("M08", 55)
wait(destroyed("T06"), { timeout = 10 })
rotate("M09", 145)
rotate("M06", 160)
wait(destroyed("T04"), { timeout = 10 })
rotate("M09", 135)
wait(destroyed("T08"), { timeout = 10 })
rotate("M06", 180)
rotate("M01", 135)
wait(destroyed("T10"), { timeout = 25 })
wait(destroyed("T02"), { timeout = 25 })
rotate("M04", 50)


