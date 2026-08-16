import { defineConfig } from 'vite'
import { svelte } from '@sveltejs/vite-plugin-svelte'

export default defineConfig({
  plugins: [svelte()],
  build: {
    cssTarget: 'chrome120',
    cssMinify: false,
  },
  server: {
    proxy: {
      '/api': 'http://localhost:8103',
      '/callback': 'http://localhost:8103'
    }
  }
})
