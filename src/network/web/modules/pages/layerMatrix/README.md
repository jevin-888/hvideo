# LayerMatrix 模块拆分说明

## 模块结构

### 已创建的模块：
- ✅ `state.js` - 状态管理（全局变量）
- ✅ `utils.js` - 工具函数（解析、颜色转换等）
- ✅ `config.js` - 配置管理（加载系统配置、应用配置等）
- ✅ `sceneManagement.js` - 场景管理（加载、保存场景等）
- ✅ `canvas.js` - 画布绘制（初始化、绘制图层、切片等）
- ✅ `dataLoader.js` - 数据加载（加载图层列表、更新图层矩阵等）
- ✅ `layerManagement.js` - 图层管理（创建、删除、选择、属性管理等）
- ✅ `sliceManagement.js` - 切片管理（创建、删除、选择、属性管理等）
- ✅ `eventHandlers.js` - 事件处理（鼠标事件、光标更新等）
- ✅ `uiRenderer.js` - UI渲染（图层矩阵、属性面板、表单渲染等）
- ✅ `qrCodeHelpers.js` - 二维码辅助（生成、预览、颜色设置等）
- ✅ `index.js` - 主入口文件（整合所有模块，提供统一接口）

### 待拆分模块

由于原文件较大（5000+行），建议继续拆分以下模块：

1. **canvas.js** - 画布相关功能
   - `initializeCanvas()` - 初始化画布
   - `drawCanvas()` - 绘制画布
   - `drawLayerRect()` - 绘制图层矩形
   - `drawSliceRect()` - 绘制切片矩形
   - `drawResizeHandles()` - 绘制调整大小控制点
   - `getCanvasScale()` - 获取画布缩放比例
   - `systemToCanvas()` - 系统坐标转画布坐标
   - `canvasToSystem()` - 画布坐标转系统坐标
   - `updateCanvasSizeByLayout()` - 更新画布尺寸

2. **layerManagement.js** - 图层管理
   - `createLayer()` - 创建图层
   - `deleteLayer()` - 删除图层
   - `applyLayerProperties()` - 应用图层属性
   - `selectLayer()` - 选择图层
   - `refreshLayerInfo()` - 刷新图层信息
   - `getLayerColorById()` - 获取图层颜色
   - `getLayerColor()` - 获取图层颜色（带选中状态）

3. **sliceManagement.js** - 切片管理
   - `createSlice()` - 创建切片
   - `deleteSlice()` - 删除切片
   - `selectSlice()` - 选择切片
   - `updateSliceInfo()` - 更新切片信息
   - `toggleSliceVisibility()` - 切换切片可见性

4. **eventHandlers.js** - 事件处理
   - `onCanvasMouseDown()` - 画布鼠标按下
   - `onCanvasMouseMove()` - 画布鼠标移动
   - `onCanvasMouseUp()` - 画布鼠标释放
   - `updateCursor()` - 更新光标样式
   - `getClickedLayer()` - 获取点击的图层
   - `getClickedSlice()` - 获取点击的切片
   - `getClickedHandle()` - 获取点击的控制点

5. **uiRenderer.js** - UI渲染
   - `renderLayerMatrix()` - 渲染图层矩阵
   - `updateLayerSelection()` - 更新图层选择
   - `showLayerProperties()` - 显示图层属性
   - `updatePropertyInputs()` - 更新属性输入框
   - `renderQRCodeDashboard()` - 渲染二维码控制面板
   - `setupQRCodeDashboardListeners()` - 设置二维码监听器
   - `setupQRCodeColorInputs()` - 设置二维码颜色输入
   - `setupLayer40ColorInputs()` - 设置图层40颜色输入

6. **dataLoader.js** - 数据加载
   - `loadAllAvailableLayers()` - 加载所有可用图层
   - `performLayerMatrixUpdate()` - 执行图层矩阵更新

## 使用方式

### 方式一：使用新的模块化入口（推荐）

```javascript
import { initializeLayerMatrix, updateLayerMatrix, selectLayer } from './layerMatrix.js';
```

### 方式二：直接使用子模块

```javascript
import * as state from './layerMatrix/state.js';
import * as utils from './layerMatrix/utils.js';
import * as config from './layerMatrix/config.js';
import * as sceneManagement from './layerMatrix/sceneManagement.js';
```

## 重构状态

### 已完成 ✅
- ✅ 所有功能模块已拆分完成（11个模块）
- ✅ 创建了模块化的入口文件 `index.js`
- ✅ 保持了原有的导出接口
- ✅ 二维码辅助模块已完整迁移（`qrCodeHelpers.js`）
- ✅ 所有模块的依赖关系已正确配置
- ✅ 所有函数签名已匹配并正确传递参数
- ✅ 无linter错误

### 模块统计
- **总模块数**: 11个
- **导出函数数**: 80+个
- **代码行数**: 从原来的5131行拆分为多个模块，每个模块职责清晰

### 模块依赖关系
```
index.js (主入口)
├── state.js (状态管理)
├── utils.js (工具函数)
├── config.js (配置管理)
├── sceneManagement.js (场景管理)
├── canvas.js (画布绘制)
├── dataLoader.js (数据加载)
├── layerManagement.js (图层管理)
├── sliceManagement.js (切片管理)
├── eventHandlers.js (事件处理)
├── uiRenderer.js (UI渲染)
└── qrCodeHelpers.js (二维码辅助)
```

### 使用建议

1. **新代码**：使用新的模块化入口 `index.js`
2. **旧代码**：原 `layerMatrix.js` 文件保留作为备份，可逐步迁移引用
3. **测试**：建议全面测试所有功能，确保迁移后功能正常
4. **清理**：确认所有功能正常后，可将原文件重命名为 `layerMatrix.old.js` 作为备份

## 注意事项

1. 状态管理模块使用 `export let`，需要在使用时通过模块引用
2. 函数参数需要传递必要的状态和依赖
3. 保持模块间的依赖关系清晰
4. 避免循环依赖
5. 部分复杂函数（如HTML生成）建议保持原实现或逐步迁移

