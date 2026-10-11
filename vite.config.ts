import { defineConfig } from 'vite';
import { viteSingleFile } from 'vite-plugin-singlefile';

// Single self-contained HTML file. It runs from any path, from file://,
// and from http://localhost. base './' keeps asset paths relative.
export default defineConfig({
  base: './',
  plugins: [viteSingleFile()],
  build: {
    target: 'es2022',
    outDir: 'dist',
    sourcemap: false,
  },
});
