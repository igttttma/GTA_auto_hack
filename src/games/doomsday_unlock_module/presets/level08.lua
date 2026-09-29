-- 预设脚本：每个调用是一步，可单步执行
-- rotate 负责定位（自动选中，到位自停），wait(destroyed) 确认战果
-- 谓词: beam.hits(id) / beam.terminal(id) / destroyed(id) / selected(id)
--       any(p1, p2) / all(p1, p2) 组合条件
-- 查询: angles() / alive()
rotate("M05", 135)
rotate("M07", 135)
wait(destroyed("T01"), { timeout = 10 })
rotate("M03", 130)
rotate("M01", 40)
rotate("M06", 165)
wait(destroyed("T06"), { timeout = 10 })
rotate("M03", 135)
wait(destroyed("T02"), { timeout = 10 })
rotate("M07", 140)
