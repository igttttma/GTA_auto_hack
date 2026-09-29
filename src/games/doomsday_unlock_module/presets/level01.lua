-- L01 预设：在模拟器里编辑，Ctrl+S 保存到此文件；实机按 F8 时自动读取最新版本
-- 风格：rotate 定位（自动选中，到位自停），wait(destroyed) 确认战果。
-- 本关光路经角棱镜级联（M04→C→M01→C→C→M03→M02），落点角度以模拟器为准。
rotate("M04", 45)
rotate("M01", 45)
rotate("M03", 135)
rotate("M05", 15)
rotate("M02", 20)
wait(destroyed("T06"), { timeout = 15 })
rotate("M02", 15)
wait(destroyed("T03"), { timeout = 10 })
rotate("M02", 180)
wait(destroyed("T07"), { timeout = 15 })
rotate("M05", 135)


