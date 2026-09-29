-- 预设脚本：每个调用是一步，可单步执行
-- rotate 负责定位（自动选中，到位自停），wait(destroyed) 确认战果
-- 谓词: beam.hits(id) / beam.terminal(id) / destroyed(id) / selected(id)
--       any(p1, p2) / all(p1, p2) 组合条件
-- 查询: angles() / alive()
rotate("M08", 180)
rotate("M03", 165)
wait(destroyed("T05"), { timeout = 10 })
rotate("M07", 40)
wait(destroyed("T02"), { timeout = 10 })
rotate("M03", 150)
wait(destroyed("T06"), { timeout = 10 })
rotate("M07", 60)
wait(destroyed("T08"), { timeout = 10 })
rotate_interval_until("M07", 55, 60, destroyed("T07"), { timeout = 20 })
rotate("M02", 135)
rotate("M04", 135)
wait(destroyed("T03"), { timeout = 10 })
rotate("M09", 75)

