#include<vector>
#include<map>
#include<set>
#include<mutex>
#include<thread>

extern void* mtalloc(size_t bytes);
extern void mtfree(void* ptr);


static const size_t MAX_BLOCK_SIZE = 1024 * 8;
static const int BINS_NUMBER = 13;
static const size_t MIN_BLOCK_SIZE = 4;

class Superblock {
public:
	
	Superblock() {
		createBlock();
	}

	~Superblock() {
		std::free(base);
	}

	void* getBlock(size_t size) {
		for (int i = block.size(); i >=0 ; --i) {
			if (!blockUsage[i] && blockSize[i] >= size) {
				blockUsage[i] = true;
				size -= blockSize[i];
				return block[i];
			}
		}
	}

	size_t releaseBlock(void*& memoryPointer) {
		for (int i = 0; i < block.size(); ++i) {
			if (block[i] == memoryPointer) {
				size += blockSize[i];
				blockUsage[i] = false;
				return blockSize[i];
			}
		}
	}

	size_t getSize() {
		return size;
	}

	bool tryAllocate(size_t size) {
		for (int i = 0; i < block.size(); ++i) {
			if (blockSize[i] >= size && blockUsage[i]) {
				return true;
			}
		}
	}
	
	bool getBlockStatus(int i) {
		return blockUsage[i];
	}



private:

	void* base;
	std::vector<void*> block;
	std::vector<size_t> blockSize;
	std::vector<bool> blockUsage;
	size_t size;

	void createBlock() {
		base = std::malloc(MAX_BLOCK_SIZE);
		size = MAX_BLOCK_SIZE;
		size_t tempSize = MAX_BLOCK_SIZE / 2;
		while (tempSize > 0) {
			void* memBlock = (void*)((char*)base + tempSize);
			block.push_back(memBlock);
			blockSize.push_back(tempSize);
			blockUsage.push_back(false);
		}
	}

};

struct blockInfo {
	Superblock* block;

	size_t getSize() {
		return block->getSize();
	}

};

bool operator > (blockInfo left, blockInfo right) {
	return left.getSize() < right.getSize();
}

bool operator < (blockInfo left, blockInfo right) {
	return left.getSize() > right.getSize();
}

class FreeBlocksHeap {
public:

	Superblock* getBlock(size_t size) {
		Superblock* block = blockSearch(size);
		if (block == nullptr) {
			return getNewBlock();
		}
	}

	void giveBlock(Superblock* block) {
		freeBlocks.insert(block);
		addFreeBlock(block);
	}

	bool isFree(Superblock* block) {
		return (freeBlocks.count(block) > 0);
	}

	void deallocateFromFree(void* ptr, Superblock* block) {
		block->releaseBlock(ptr);
		updateBins(block);
	}

private:

	std::vector<std::set<blockInfo>> bins;
	std::set<Superblock*> freeBlocks;
	std::set<Superblock*> allocatedBlocks;

	Superblock* getSuperBlock(size_t size) {
		Superblock* block = blockSearch(size);
		if (block != nullptr) {
			return block;
		}
		return getNewBlock();
	}

	Superblock* getNewBlock() {
		Superblock* block = new Superblock;
		allocatedBlocks.insert(block);
		return block;
	}

	void addFreeBlock(Superblock* block) {
		freeBlocks.insert(block);
		for (int i = 0; i < BINS_NUMBER; ++i) {
			if (block->getBlockStatus(i)) {
				bins[i].insert({ block });
			}
		}
	}

	Superblock* blockSearch(size_t size) {
		size_t size2 = 1;
		int log = 0;
		while (size2 <  size) {
			size2 *= 2;
			log++;
		}
		for (int i = log; i < bins.size(); ++i) {
			if (bins[i].size() > 0) {
				Superblock* block = (*bins[i].begin()).block;
				return block;
			}
		}
		return nullptr;
	}

	void clearBins(blockInfo inf) {
		for (int i = 0; i < bins.size(); ++i) {
			bins[i].erase(inf);
		}
	}

	void updateBins(Superblock* block) {
		for (int i = 0; i < bins.size(); ++i) {
			if (block->getBlockStatus(i)) {
				bins[i].insert({ block });
			}
			else {
				bins[i].erase({ block });
			}
		}
	}
};


FreeBlocksHeap blockHeap;

class ThreadHeap {
public:


	ThreadHeap(std::map<void*, Superblock*>* _ptrToBlock,
		std::map<Superblock*, ThreadHeap*>* _blockToHeap) {
		ptrToBlock = _ptrToBlock;
		blockToHeap = _blockToHeap;
	}

	int getSize() {
		return assignedThreads.size();
	}

	void assignThead(std::thread::id id) {
		assignedThreads.insert(id);
	}

	void deassignThread(std::thread::id id) {
		assignedThreads.erase(id);
	}

	void* allocate(size_t size) {
		std::unique_lock<std::mutex> lock(operationMutex);
		Superblock* block = findBlock(size);
		if (block == nullptr) {
			block = blockHeap.getBlock(size);
			blockToHeap->at(block) = this;
		}
		void* ptr = block->getBlock(size);
		ptrToBlock->at(ptr) = block;
		updateMemory(size, true);
		updateBins(block);
		return ptr;
	}

	void free(void* ptr) {
		std::unique_lock<std::mutex> lock(operationMutex);
		Superblock* block = ptrToBlock->at(ptr);
		size_t size = block->releaseBlock(ptr);
		updateMemory(size, false);
		if (checkIfEmpty()) {
			returnBlock();
		}

	}

	void returnBlock() {
		Superblock* block = *blocks.begin();
		blocks.erase(block);
		blockHeap.giveBlock(block);
	}

private:

	size_t usedMemory = 0;
	std::mutex operationMutex;
	std::vector<std::set<blockInfo>> bins;
	std::set<Superblock*> blocks;
	std::map<void*, Superblock*>* ptrToBlock;
	std::map<Superblock*, ThreadHeap*>* blockToHeap;
	std::set<std::thread::id> assignedThreads;

	bool checkIfEmpty() {
		long double alpha = (long double)usedMemory / (long double)getMaxMemory();
		if (alpha > 0.8) {
			return true;
		}
		return false;
	}

	void updateMemory(size_t size, bool alloc) {
		size_t size2 = 1;
		int log = 0;
		while (size2 < size) {
			size2 *= 2;
			log++;
		}
		if (alloc) {
			usedMemory += size2;
		}
		else {
			usedMemory -= size2;
		}
	}

	void updateOnDeletion(Superblock* block) {
		size_t size = MAX_BLOCK_SIZE - block->getSize();
		usedMemory -= size;
	}

	size_t getMaxMemory() {
		return blocks.size() * MAX_BLOCK_SIZE;
	}

	void updateBins(Superblock* block) {
		for (int i = 0; i < bins.size(); ++i) {
			if (block->getBlockStatus(i)) {
				bins[i].insert({ block });
			}
			else {
				bins[i].erase({ block });
			}
		}
	}

	void clearBins(blockInfo inf) {
		for (int i = 0; i < bins.size(); ++i) {
			bins[i].erase(inf);
		}
	}

	Superblock* findBlock(size_t size) {
		size_t size2 = 1;
		int log = 0;
		while (size2 < size) {
			size2 *= 2;
			log++;
		}
		for (int i = log; i < bins.size(); ++i) {
			if (bins[i].size() > 0) {
				Superblock* block = (*bins[i].begin()).block;
				return block;
			}
		}
		return nullptr;
	}

};


class GlobalHeap {

public:

	GlobalHeap() {
		int num = std::thread::hardware_concurrency() * 2;
		if (num == 0) {
			num = 16;
		}
		for (int i = 0; i < num; ++i) {
			threadHeaps.push_back(new ThreadHeap{&ptrToBlock,&blockToHeap});
		}
	}

	/*~GlobalHeap() {
		for (auto obj : threadHeaps) {
			delete obj;
		}
		for (auto block : allocatedBlocks) {
			delete block;
		}
		for (auto ptr : allocatedMemory) {
			delete ptr;
		}
	}
	*/
	void* allocate(size_t size) {
		if (size > MAX_BLOCK_SIZE / 2) {
			void* memoryBlock = std::malloc(size);
			allocatedMemory.insert(memoryBlock);
			return memoryBlock;
		}
		return allocateToThread(size);
	}

	void free(void* pointer) {
		if (allocatedMemory.count(pointer) != 0) {
			allocatedMemory.erase(pointer);
			std::free(pointer);
		}
		releaseFromThread(pointer);
	}

	void regThread(std::thread::id id) {
		int heapId = getMinHeap();
		threadMap[id] = heapId;
		threadHeaps[heapId]->assignThead(id);
	}

	void deredThread(std::thread::id id) {
		int heapId = threadMap[id];
		threadHeaps[heapId]->deassignThread(id);

	}

private:

	std::vector<std::set<blockInfo>> bins;
	std::set<void*> allocatedMemory;
	
	std::map<void*, Superblock*> ptrToBlock;
	std::map<Superblock*, ThreadHeap*> blockToHeap;
	std::map<std::thread::id, int> threadMap;

	std::vector<ThreadHeap*> threadHeaps;

	std::mutex operationMutex;

	

	void* allocateToThread(size_t size) {
		std::thread::id id = std::this_thread::get_id();
		ThreadHeap* heap = threadHeaps[threadMap[id]];
		return heap->allocate(size);
	}

	void releaseFromThread(void* ptr) {
		Superblock* block = ptrToBlock[ptr];
		if (blockHeap.isFree(block)) {
			blockHeap.deallocateFromFree(ptr, block);
			return;
		}
		ThreadHeap* heap = blockToHeap[block];
		heap->free(ptr);
	}

	



	int getMinHeap() {
		int min = 10e6;
		int minId = 0;
		for (int i = 0; i < threadHeaps.size(); ++i) {
			if (threadHeaps[i]->getSize() < min) {
				min = threadHeaps[i]->getSize();
				minId = i;
			}
		}
		return minId;
	}

};

static GlobalHeap global;




class ThreadRegister {
public:
	ThreadRegister() {
		global.regThread(std::this_thread::get_id());
	}

	~ThreadRegister() {
		global.deredThread(std::this_thread::get_id());
	}
};

static thread_local ThreadRegister regObj;

extern void* mtalloc(size_t bytes) {
	return global.allocate(bytes);
}

extern void mtfree(void* ptr) {
	global.free(ptr);
}
