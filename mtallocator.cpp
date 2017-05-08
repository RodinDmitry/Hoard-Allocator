#include<vector>
#include<map>
#include<set>
#include<mutex>
#include<thread>

extern void* mtalloc(size_t bytes);
extern void mtfree(void* ptr);

static const size_t MAX_BLOCK_SIZE = 1024 * 8;
static const int BINS_NUMBER = 13;
static const size_t MIN_BLOCK_THRESHOLD = 5;
static const long double MEMORY_FRACTION_THRESHOLD = 0.25;


class Superblock {
public:
	
	Superblock() {
		createBlock();
	}

	~Superblock() {
		std::free(base);
	}

	void* getBlock(size_t size) {
		unsigned int minBlockId = block.size();
		for (unsigned int i = 0; i < block.size() ; ++i) {
			if (!blockUsage[i] && blockSize[i] >= size) {
				minBlockId = i;
			}
		}
		blockUsage[minBlockId] = true;
		return block[minBlockId];
	}

	size_t releaseBlock(void*& memoryPointer) {
		for (unsigned int i = 0; i < block.size(); ++i) {
			if (block[i] == memoryPointer) {
				size += blockSize[i];
				blockUsage[i] = false;
				return blockSize[i];
			}
		}
		return 0;
	}

	size_t getSize() {
		return size;
	}

	bool tryAllocate(size_t size) {
		for (unsigned int i = 0; i < block.size(); ++i) {
			if (blockSize[i] >= size && blockUsage[i]) {
				return true;
			}
		}
		return false;
	}
	
	bool getBlockStatus(int i) {
		return blockUsage[12 - i];
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
			tempSize /= 2;
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

	FreeBlocksHeap() {
		for (int i = 0; i < BINS_NUMBER; ++i) {
			bins.push_back(std::set <blockInfo>());
		}
	}

	~FreeBlocksHeap() {
		for (auto block : allocatedBlocks) {
			delete block;
		}
	}

	Superblock* getBlock(size_t size) {
		std::unique_lock<std::mutex> lock(operationMutex);
		Superblock* block = blockSearch(size);
		if (block == nullptr) {
			return getNewBlock();
		}
		freeBlocks.erase(block);
		return block;
	}

	void giveBlock(Superblock* block) {
		std::unique_lock<std::mutex> lock(operationMutex);
		addFreeBlock(block);
	}

	bool isFree(Superblock* block) {
		std::unique_lock<std::mutex> lock(operationMutex);
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
	std::mutex operationMutex;

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
			if (!block->getBlockStatus(i)) {
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
		for (unsigned int i = log; i < bins.size(); ++i) {
			if (bins[i].size() > 0) {
				Superblock* block = (*bins[i].begin()).block;
				return block;
			}
		}
		return nullptr;
	}

	void clearBins(blockInfo inf) {
		for (unsigned int i = 0; i < bins.size(); ++i) {
			bins[i].erase(inf);
		}
	}

	void updateBins(Superblock* block) {
		for (unsigned int i = 0; i < bins.size(); ++i) {
			if (!block->getBlockStatus(i)) {
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
		for (int i = 0; i < BINS_NUMBER; ++i) {
			bins.push_back(std::set <blockInfo>());
		}
	}

	int getSize() {
		std::unique_lock<std::mutex> lock(operationMutex);
		return assignedThreads.size();
	}

	void assignThead(std::thread::id id) {
		std::unique_lock<std::mutex> lock(operationMutex);
		assignedThreads.insert(id);
	}

	void deassignThread(std::thread::id id) {
		std::unique_lock<std::mutex> lock(operationMutex);
		assignedThreads.erase(id);
	}

	void* allocate(size_t size) {
		std::unique_lock<std::mutex> lock(operationMutex);
		Superblock* block = findBlock(size);
		if (block == nullptr) {
			block = blockHeap.getBlock(size);
			blocks.insert(block);
			blockToHeap->insert(std::pair<Superblock*,ThreadHeap*>(block,this));
		}
		void* ptr = block->getBlock(size);
		(*ptrToBlock)[ptr] = block;
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


private:

	size_t usedMemory = 0;
	std::mutex operationMutex;
	std::vector<std::set<blockInfo>> bins;
	std::set<Superblock*> blocks;
	std::map<void*, Superblock*>* ptrToBlock;
	std::map<Superblock*, ThreadHeap*>* blockToHeap;
	std::set<std::thread::id> assignedThreads;


	void returnBlock() {
		Superblock* block = *blocks.begin();
		blocks.erase(block);
		clearBins({ block });
		blockHeap.giveBlock(block);
	}

	bool checkIfEmpty() {
		long double alpha = (long double)usedMemory / (long double)getMaxMemory();
		if (alpha < MEMORY_FRACTION_THRESHOLD
			&& (usedMemory < (getMaxMemory() 
				- MIN_BLOCK_THRESHOLD * MAX_BLOCK_SIZE))) {
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
		for (unsigned int i = 0; i < bins.size(); ++i) {
			if (!block->getBlockStatus(i)) {
				bins[i].insert({ block });
			}
			else {
				bins[i].erase({ block });
			}
		}
	}

	void clearBins(blockInfo inf) {
		for (unsigned int i = 0; i < bins.size(); ++i) {
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
		for (unsigned int i = log; i < bins.size(); ++i) {
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

	~GlobalHeap() {
		for (auto obj : threadHeaps) {
			delete obj;
		}
		for (auto ptr : allocatedMemory) {
			std::free(ptr);
		}
	}
	
	void* allocate(size_t size) {
		if (size > MAX_BLOCK_SIZE / 2) {
			std::unique_lock<std::mutex> lock(operationMutex);
			void* memoryBlock = std::malloc(size);
			allocatedMemory.insert(memoryBlock);
			return memoryBlock;
		}
		return allocateToThread(size);
	}

	void free(void* pointer) {
		if (allocatedMemory.count(pointer) != 0) {
			std::unique_lock<std::mutex> lock(operationMutex);
			allocatedMemory.erase(pointer);
			std::free(pointer);
		}
		releaseFromThread(pointer);
	}

	void regThread(std::thread::id id) {
		std::unique_lock<std::mutex> lock(operationMutex);
		int heapId = getMinHeap();
		threadMap[id] = heapId;
		threadHeaps[heapId]->assignThead(id);
	}

	void deredThread(std::thread::id id) {
		std::unique_lock<std::mutex> lock(operationMutex);
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
		std::unique_lock<std::mutex> lock(operationMutex);
		Superblock* block = ptrToBlock[ptr];
		if (blockHeap.isFree(block)) {
			blockHeap.deallocateFromFree(ptr, block);
			return;
		}
		ThreadHeap* heap = blockToHeap[block];
		lock.unlock();
		heap->free(ptr);
	}

	int getMinHeap() {
		int min = 10e6;
		int minId = 0;
		for (unsigned int i = 0; i < threadHeaps.size(); ++i) {
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
