#pragma once

class Engine
{
  public:
    static void Init();
    static void Uninit();
    static void Run();

  private:
    static void Update();
    static void Draw();
};
