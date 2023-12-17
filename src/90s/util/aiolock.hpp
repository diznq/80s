#pragma once
#include <queue>
#include "../aiopromise.hpp"

namespace s90 {
    namespace util {
        class aiolock {
            std::queue<aiopromise<bool>> waiters;
            int sem = 1;
        public:
            ~aiolock() {
                while(waiters.size() > 0) {
                    auto w = waiters.front();
                    waiters.pop();
                    w.resolve(false);
                }
            }

            aiopromise<bool> lock() {
                aiopromise<bool> result;
                if(sem == 1 && waiters.size() == 0) {
                    sem = 0;
                    result.resolve(true);
                } else if(sem == 1) {
                    sem = 0;
                    auto first = waiters.front();
                    waiters.pop();
                    waiters.push(result);
                    first.resolve(true);
                } else {
                    waiters.push(result);
                }
                return result;
            }

            void unlock() {
                if(waiters.size() > 0) {
                    sem = 0;
                    auto first = waiters.front();
                    waiters.pop();
                    first.resolve(true);
                } else {
                    sem = 1;
                }
            }
        };
    }
}