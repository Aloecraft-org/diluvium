const path = require('path');
const webpack = require('webpack');
const HtmlWebpackPlugin = require('html-webpack-plugin');
const CopyWebpackPlugin = require('copy-webpack-plugin');

// Where the release mirror lives, relative to the served site.
//
// In production this is same-origin: nginx roots diluvium.aloecraft.org at
// /var/www/html/diluvium/, and the mirror writes into release/ underneath it,
// so '/release' resolves without a second host being involved.
//
// For local development against the real mirror, point it at the deployed
// one -- /release/ sends Access-Control-Allow-Origin: *, so a dev server on
// localhost can read it:
//
//   RELEASE_BASE=https://diluvium.aloecraft.org/release npm start
//
const RELEASE_BASE = process.env.RELEASE_BASE || '/release';

module.exports = (env, argv) => ({
  entry: './src/app.js',
  output: {
    filename: '[name].[contenthash:8].js',
    path: path.resolve(__dirname, 'dist'),
    clean: true,
  },
  module: {
    rules: [
      { test: /\.css$/i, use: ['style-loader', 'css-loader'] },
      { test: /\.(png|svg|ico)$/i, type: 'asset/resource' },
    ],
  },
  plugins: [
    new webpack.DefinePlugin({
      RELEASE_BASE: JSON.stringify(RELEASE_BASE),
    }),

    new HtmlWebpackPlugin({
      template: './public/index.html',
      filename: 'index.html',
      favicon: './public/favicon.ico',
      minify: argv.mode === 'production' && {
        collapseWhitespace: true,
        removeComments: true,
        // The REPL's <pre> samples are whitespace-significant.
        conservativeCollapse: false,
        minifyCSS: true,
      },
    }),

    new CopyWebpackPlugin({
      patterns: [
        // The installer is served at /start with no extension, because the
        // whole point is that `curl … /start | sh` is short enough to type
        // from memory. nginx gives it text/plain; see the launchkit vhost
        // drop-in. It is deliberately NOT run through webpack -- it is a
        // shell script, not a module, and its bytes must reach curl intact.
        { from: 'public/start.sh', to: 'start', toType: 'file' },
        { from: 'public/robots.txt', to: 'robots.txt' },
      ],
    }),
  ],
  devServer: {
    static: { directory: path.resolve(__dirname, 'dist') },
    port: 8081,
    hot: false,
  },
  // The kernel is ~900 KB and xterm is not small; the defaults would warn on
  // every build about a page whose weight is a deliberate choice.
  performance: { maxAssetSize: 1024 * 1024, maxEntrypointSize: 1024 * 1024 },
  devtool: argv.mode === 'production' ? false : 'source-map',
});
