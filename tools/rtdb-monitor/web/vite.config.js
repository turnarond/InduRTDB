import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// 生产：FastAPI 以相对路径托管 dist/（base './'）。
// 开发：npm run dev 时把 /api 与 /ws 代理到后端（默认 8090）。
export default defineConfig({
  plugins: [vue()],
  base: './',
  server: {
    proxy: {
      '/api': { target: 'http://127.0.0.1:8090', changeOrigin: true },
      '/ws': { target: 'ws://127.0.0.1:8090', ws: true, changeOrigin: true }
    }
  }
})
