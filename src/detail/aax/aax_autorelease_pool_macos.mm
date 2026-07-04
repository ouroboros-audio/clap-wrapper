#import <Foundation/Foundation.h>

#include "AAX_CAutoreleasePool.h"

AAX_CAutoreleasePool::AAX_CAutoreleasePool()
    : mAutoreleasePool([[NSAutoreleasePool alloc] init])
{
}

AAX_CAutoreleasePool::~AAX_CAutoreleasePool()
{
  [(NSAutoreleasePool *)mAutoreleasePool drain];
  mAutoreleasePool = nullptr;
}
