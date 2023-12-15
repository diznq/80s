#pragma once
#include <queue>
#include "../aiopromise.hpp"

namespace s90 {
    namespace util {
        class aiolock {
            std::queue<aiopromise<nil>> waiters;
            int sem = 1;
        public:
            aiopromise<nil> lock() {
                aiopromise<nil> result;
                if(sem == 1 && waiters.size() == 0) {
                    sem = 0;
                    result.resolve({});
                } else if(sem == 1) {
                    sem = 0;
                    auto first = waiters.front();
                    waiters.pop();
                    waiters.push(result);
                    first.resolve({});
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
                    first.resolve({});
                } else {
                    sem = 1;
                }
            }
        };
    }
}