# 诊断字体来源

RVT 查看器使用 [Daniel Hepper 的 font8x8](https://github.com/dhepper/font8x8) 基础 ASCII 字形，
由引擎转换成既有 FontData 数据并通过 FontSystem 绘制，不引入第二套字体渲染器。

仓库固定保存 `third_party/font8x8/font8x8_basic.h` 及 README；该源文件 SHA256 为
`98d01e0312b15f69c4919e561e5c1a5c215480e1d7f5cda1d18616db1a3127b7`。
源注释和 README 声明 Public Domain，保留 Daniel Hepper、Marcel Sondaar 和 IBM VGA 字体来源说明。
上游固定提交与本地集成调整见 `third_party/font8x8/source.md`；README 仅整理空白与标题格式。更新时需重新核对字形来源和文件摘要。
