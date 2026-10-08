#include 
struct Vec2{
  int x;
  int y;
}

struct UILayout{
  
  Vec2 win_size;

  Vec2 header_origin;
  Vec2 header_size;
  
  Vec2 footer_origin;
  Vec2 footer_size;

  int padding;


  //irgendwie die display dimensions von terminal 
  //nehmen um dynamische breiten nutzen zu können

  void SetpUiDimensions(){
    
    header_origin = {0 + padding, 0 + padding};
    header_size = {3, }
  }

}

