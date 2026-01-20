const path = require('path');
const HtmlWebpackPlugin = require('html-webpack-plugin');
const CopyWebpackPlugin = require('copy-webpack-plugin');

module.exports = {
  entry: './src/app.js',
  output: {
    filename: 'bundle.js',
    path: path.resolve(__dirname, 'dist'),
  },
  module: {
    rules: [
      {
        test: /\.css$/i,
        use: ['style-loader', 'css-loader', 'postcss-loader'],
      },
      {
        test: /\.wasm$/,
        type: 'asset/resource',
        generator: {
          filename: '[name][ext]'
        }
      }
    ],
  },

  plugins: [
    new HtmlWebpackPlugin({
      template: './public/index.html',
      filename: 'index.html',
      favicon: './public/favicon.ico'
    }),

    new CopyWebpackPlugin({
      patterns: [
        { from: 'src/libdiluvium.wasm', to: '' },
      ],
    }),
  ],
  mode: 'development',
};
