#pragma once


//--------------------------------------------------------------------------------------------------
// This implements all graphical user interface of the ray tracer.
class Raytracer; // Forward declaration

class GUI
{
public:
  GUI(Raytracer* _s)
      : _se(_s)
  {
  }
  void render();
  void showBusyWindow();

private:
  bool           guiRayTracing();
  bool           guiTonemapper();
  bool           guiEnvironment();
  void           loadSceneWindow();


  Raytracer* _se{nullptr};
};

