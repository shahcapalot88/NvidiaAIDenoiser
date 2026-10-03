# NVIDIA AI Denoiser Fork, now comes with it's own GUI.

**CLI - Provided by Declan Rusell.**

**GUI - Provided by Subhajit Maji.**
____________________________________


**You will require an Nvidia driver of at least 565.xx or higher and an Nvidia GPU of Maxwell architecture or newer to use the OptiX denoiser.**


You need to at least have an output set for the app to run. If you also have them, you can add an albedo AOV or albedo and normal AOVs to improve the denoising. All images should be the same resolutions, not meeting this requirement will lead to unexpected results (likely a crash).

For best results provide as many of the AOVs as possible to the denoiser. Generally the more information the denoiser has to work with the better. The denoiser also prefers images rendered with a box filter or by using FIS.

## Examples
Here is a quick example scene that uses the images that can be found in the image folder of this repository.

### Noisy image
<p align="center">
  <img src="https://github.com/DeclanRussell/NvidiaAIDenoiser/blob/master/images/RGBA.png" alt="test"/>
</p>

### Denoised output
<p align="center">
  <img src="https://github.com/DeclanRussell/NvidiaAIDenoiser/blob/master/images/RGBA_denoised.png" alt="denoise_test"/>
</p>


# License info
This project is shared under the [MIT License](https://mit-license.org/).
