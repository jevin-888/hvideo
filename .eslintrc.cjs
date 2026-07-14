module.exports = {
  root: true,
  env: {
    browser: true,
    es2021: true,
  },
  parserOptions: {
    ecmaVersion: 'latest',
    sourceType: 'module',
  },
  plugins: ['unused-imports'],
  extends: ['eslint:recommended'],
  ignorePatterns: [
    'node_modules/**',
    'app/**',
    'dist/**',
    'src/network/web/huoshanKTV/utils/libs/**',
    'src/network/web/huoshanVOD/utils/libs/**',
    'src/network/web/qrcode.min.js',
  ],
  rules: {
    // 关闭默认的 no-unused-vars，交给 unused-imports 处理
    'no-unused-vars': 'off',

    // 移除未使用的 import
    'unused-imports/no-unused-imports': 'error',

    // 移除未使用的变量 / 函数声明
    'unused-imports/no-unused-vars': [
      'error',
      {
        vars: 'all',
        args: 'none',
        ignoreRestSiblings: true,
      },
    ],

    // 检查不可达代码
    'no-unreachable': 'error',

    // 以下规则与“死代码”关系不大，先关闭避免阻塞自动清理
    'no-empty': 'off',
    'no-prototype-builtins': 'off',
    'no-dupe-class-members': 'off',
  },
  overrides: [
    {
      files: [
        'src/network/web/huoshanKTV/check-build-files.js',
        'src/network/web/huoshanKTV/scripts/**/*.js',
      ],
      env: {
        node: true,
        commonjs: true,
      },
    },
  ],
};
