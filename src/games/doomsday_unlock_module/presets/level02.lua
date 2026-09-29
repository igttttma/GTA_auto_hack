-- 预设脚本：每个调用是一步，可单步执行
-- rotate 负责定位（自动选中，到位自停），wait(destroyed) 确认战果
-- 谓词: beam.hits(id) / beam.terminal(id) / destroyed(id) / selected(id)
--       any(p1, p2) / all(p1, p2) 组合条件
-- 查询: angles() / alive()
rotate("M03", 10)
wait(destroyed("T04"), { timeout = 10 })
rotate("M03", 150)
rotate("M04", 45)
wait(destroyed("T03"), { timeout = 10 })
rotate("M03", 135)
rotate("M05", 25)
wait(destroyed("T01"), { timeout = 10 })
rotate("M02", 25)
wait(destroyed("T02"), { timeout = 10 })
rotate("M05", 70)

