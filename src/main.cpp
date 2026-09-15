#include <iostream>
#include "trade_event.hpp"
#include <string>

std::string banner = "KAITO v0.1";
std::string separator = "\n---------\n";


int main(){
  std::cout << separator << banner << separator << std::endl;

  TradeEvent event;

  std::cout << "EVENT SIZE: " << sizeof(event) << " BYTE" << std::endl;
  std::cout << "EVENT ALIGNMENT: " << alignof(event) << " BYTE" << std::endl;


  return 0;
}
