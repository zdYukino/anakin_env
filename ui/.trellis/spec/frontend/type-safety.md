# 类型与格式
tsconfig开启strict和verbatimModuleSyntax。协议类型在core/types.ts，外部JSON在边界验证为unknown/Record后归一；组件使用已归一的类型。
core/format.ts集中处理精度、科学计数法与不可用值；robot.findJoints按名称绑定，null/非有限值保留为不可用而非Number(null)=0。
导入Worker区分preview/convert/complete/error，时间单位s/ms/ns/iso/ros。错误时间阻止导入，逆序/重复/缺值按importer中的明确策略处理和提示。
