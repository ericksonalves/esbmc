#include <string>
#include <cassert>
using namespace std;

int main(){
  string aux;
  aux = 'D';
  string str1, str2;
  str1 = string("D");
  str2 = string(str1);
  assert(aux <= str2);
}
