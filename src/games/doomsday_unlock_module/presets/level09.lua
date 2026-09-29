-- 预设脚本：每个调用是一步，可单步执行
-- rotate 负责定位（自动选中，到位自停），wait(destroyed) 确认战果
-- 谓词: beam.hits(id) / beam.terminal(id) / destroyed(id) / selected(id)
--       any(p1, p2) / all(p1, p2) 组合条件
-- 查询: angles() / alive()
wait(destroyed("T01"), { timeout = 10 })
rotate("M03", 175)
rotate("M05", 20)
wait(destroyed("T06"), { timeout = 10 })
rotate("M03", 20)
rotate("M01", 30)
wait(destroyed("T05"), { timeout = 10 })
rotate("M01", 45)
rotate("M04", 135)
