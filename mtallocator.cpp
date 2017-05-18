#include<vector>
#include<map>
#include<set>
#include<mutex>
#include<thread>
#include<algorithm>


extern void* mtalloc(size_t bytes);
extern void mtfree(void* ptr);

static const size_t MAX_BLOCK_SIZE = 1024 * 8;
static const size_t MIN_BLOCK_SIZE = 8;
static const size_t MIN_BLOCK_THRESHOLD = 5;
static const long double MEMORY_FRACTION_THRESHOLD = 4;

// std::thread::hardware_concurrency может вернуть 0 в некоторых случаях
static const size_t HEAPS_NUMBER = 
	std::max(std::thread::hardware_concurrency() * 2,(unsigned int)8);
thread_local const size_t THREAD_ID = 
	std::hash<std::thread::id>()(std::this_thread::get_id()) % HEAPS_NUMBER;


class Superblock {
public:

	Superblock(size_t size) {
		base = std::malloc(MAX_BLOCK_SIZE);
		usedMemory = 0;
		ownerHeap = -1;
		blockSize = normalize(size);
		totalBlocks = MAX_BLOCK_SIZE / blockSize;
		currentPosition = totalBlocks - 1;
		for (int i = 0; i < totalBlocks; i++) {
			freeBlocks.push_back(i * blockSize);
		}
	}

	~Superblock() {
		std::free(base);
	}

	void* getBlock() {
		if (currentPosition == totalBlocks) {
			return nullptr;
		}
		auto pt = freeBlocks[currentPosition];
		currentPosition--;
		return (char*)base + pt;
	}

	void releaseBlock(void* block) {
		char* ptr = (char*)block;
		size_t position = ptr - (char*)base;
		currentPosition++;
		freeBlocks[currentPosition] = position;
	}

	size_t getSize() {
		return blockSize;
	}

	
	
	int ownerHeap;
	int totalBlocks;
	int currentPosition;
	size_t usedMemory;
	
	std::mutex operationMutex;

private:

	void* base;
	std::vector<size_t> freeBlocks;
	size_t blockSize;

	size_t normalize(size_t size) {
		size_t size2 = MIN_BLOCK_SIZE;
		while (size2 < size) {
			size2 *= 2;
		}
		return size2;
	}
};

class SuperblockOwner {
public:
	~SuperblockOwner() {
		for (auto block : blocks) {
			delete block;
		}
	}

	Superblock* getNewBlock(size_t blockSize) {
		Superblock* block = new Superblock(blockSize);
		blocks.push_back(block);
		return block;
	}

	static SuperblockOwner& getInstance() {
		static SuperblockOwner instance;
		return instance;
	}

private:
	std::vector<Superblock*> blocks;
};

class Bin {
public:
	Bin() {
		allocatedMemory = 0;
		usedMemory = 0;
	}

	void addSuperblock(Superblock* block) {

		if (block->currentPosition == -1) {
			fullBlocks.push_back(block);
			return;
		}
		fullnessGroups.push_back(block);
	}

	Superblock* getBlock() {

		if (fullnessGroups.empty()) {
			return nullptr;
		}
		Superblock* block = fullnessGroups.front();
		std::swap(fullnessGroups[0], fullnessGroups.back());
		fullnessGroups.pop_back();
		return block;
	}

	void deallocate(Superblock* block, void *ptr) {
		if (block->currentPosition == -1) {
			block->releaseBlock(ptr);
			auto it = std::find(fullBlocks.begin(), fullBlocks.end(), block) - fullBlocks.begin();
			std::swap(fullBlocks.back(), fullBlocks[it]);
			fullBlocks.pop_back();
			fullnessGroups.push_back(block);
			return;
		}
		block->releaseBlock(ptr);
	}

	std::pair<Superblock*, void*> scan() {
		if (fullnessGroups.empty()) {
			return { nullptr, nullptr };
		}

		Superblock* superblock = fullnessGroups.back();
		fullnessGroups.pop_back();
		auto block = superblock->getBlock();
		return { superblock, block };
	}

	void updateMemory(size_t used,size_t allocated, bool signum) {
		if (signum) {
			usedMemory += used;
			allocatedMemory += allocated;
		}
		else {
			usedMemory -= used;
			allocatedMemory -= allocated;
		}
	}

	size_t getUsed() {
		return usedMemory;
	}

	size_t getAllocated() {
		return allocatedMemory;
	}

private:

	size_t allocatedMemory;
	size_t usedMemory;
	size_t iterator = 0;

	std::vector<Superblock*> fullnessGroups;
	std::vector<Superblock*> fullBlocks;

};

static SuperblockOwner blockOwner;

class ThreadHeap {
public:
	std::mutex operationMutex;

	ThreadHeap(size_t heapId) {
		id = heapId;
		mainHeap = nullptr;
		size_t curSize = 3;
		while (curSize <= MAX_BLOCK_SIZE / 2) {
			bins.push_back(Bin());
			curSize *= 2;
		}
	}

	ThreadHeap(ThreadHeap* _mainHeap,size_t heapId) {
		id = heapId;
		mainHeap = _mainHeap;
		size_t curSize = 3;
		while (curSize <= MAX_BLOCK_SIZE / 2) {
			bins.push_back(Bin());
			curSize *= 2;
		}
	}

	void* allocate(size_t size) {
		std::unique_lock<std::mutex> lock(operationMutex);
		Bin& currentBin = findBin(size);
		std::pair<Superblock*, void*> allocationInfo = currentBin.scan();
		if (allocationInfo.first == nullptr) {
			std::unique_lock<std::mutex> lockMain(mainHeap->operationMutex);
			Bin& mainBin = mainHeap->findBin(size);
			std::pair<Superblock*, void*> globalAllocationInfo = mainBin.scan();
			if (globalAllocationInfo.first != nullptr) {
				allocationInfo.first = globalAllocationInfo.first;
				allocationInfo.second = globalAllocationInfo.second;
				allocationInfo.first->ownerHeap = id;
				mainBin.updateMemory(allocationInfo.first->usedMemory,MAX_BLOCK_SIZE, false);
				currentBin.updateMemory(allocationInfo.first->usedMemory,MAX_BLOCK_SIZE, true);
			}
			else {
				allocationInfo.first = SuperblockOwner::getInstance().getNewBlock(size);
				allocationInfo.first->ownerHeap = id;
				allocationInfo.second = allocationInfo.first->getBlock();
				currentBin.updateMemory(0, MAX_BLOCK_SIZE, true);
			}
		}
		currentBin.addSuperblock(allocationInfo.first);
		information* info = (information*)allocationInfo.second;
		info->owner = allocationInfo.first;
		return (void*)((char*)info + sizeof(information));
	}

	void deallocate(void* ptr,Superblock* block) {
		Bin& currentBin = this->findBin(block->getSize());
		block->usedMemory -= block->getSize();
		currentBin.updateMemory(block->getSize(), 0, false);
		currentBin.deallocate(block, ptr);
		if (this->id == -1) {
			this->operationMutex.unlock();
			return;
		}
		tryReturnBlock(currentBin, block);
		this->operationMutex.unlock();
	}



private:

	struct information {
		void* owner;
	};

	std::vector<Bin> bins;
	ThreadHeap* mainHeap;
	int id;

	size_t getBinId(size_t val) {
		size_t log = 3;
		size_t approx = MIN_BLOCK_SIZE;
		while (approx < val) {
			approx *= 2;
			log++;
		}
		return log - 3;
	}

	size_t getSize(size_t val) {
		size_t log = 3;
		size_t approx = MIN_BLOCK_SIZE;
		while (approx < val) {
			approx *= 2;
			log++;
		}
		return approx;
	}

	Bin& findBin(size_t size) {
		return bins[getBinId(size)];
	}

	void tryReturnBlock(Bin& bin,Superblock* block) {
		if ((bin.getUsed() < bin.getAllocated() - MAX_BLOCK_SIZE * MIN_BLOCK_THRESHOLD) &&
			bin.getUsed() * MEMORY_FRACTION_THRESHOLD < bin.getAllocated()) {
			std::unique_lock<std::mutex> lock(mainHeap->operationMutex);
			Bin& mainBin = mainHeap->findBin(block->getSize());
			Superblock* emptiestBlock = bin.getBlock();
			if (emptiestBlock != nullptr) {
				emptiestBlock->ownerHeap = -1;
				mainBin.updateMemory(emptiestBlock->usedMemory, MAX_BLOCK_SIZE, true);
				bin.updateMemory(emptiestBlock->usedMemory, MAX_BLOCK_SIZE, false);
				mainBin.addSuperblock(emptiestBlock);
			}
		}
	}
};

class Controller {
public:

	Controller() {
		mainHeap = new ThreadHeap(-1);
		for (unsigned int i = 0; i < HEAPS_NUMBER; i++) {
			heaps.push_back(new ThreadHeap(mainHeap,i));
		}
	}

	~Controller() {
		for (auto heap : heaps) {
			delete heap;
		}
		delete mainHeap;
	}

	void* allocate(size_t size) {
		size_t totalSize = size + OFFSET;
		if (totalSize > MAX_BLOCK_SIZE / 2) {
			void* ptr = std::malloc(totalSize);
			if (ptr == nullptr) {
				return nullptr;
			}
			information* info = (information*)ptr;
			info->owner = nullptr;
			MemoryController::getInstance().remember(ptr);
			return (void*)((char*)ptr + OFFSET);
		}
		return heaps[THREAD_ID]->allocate(totalSize); 
	}

	void deallocate(void* ptr) {
		if (ptr == nullptr) {
			return;
		}
		information* info = (information*)((char*)ptr - OFFSET);
		if (info->owner == nullptr) {
			std::free(info);
			MemoryController::getInstance().forget(info);
			return;
		}
		ThreadHeap* ownerHeap = findOwner((Superblock*)info->owner);
		ownerHeap->deallocate(info, (Superblock*)info->owner);
	}

	static Controller& getInstance() {
		static Controller instance;
		return instance;
	}

private:

	struct information {
		void* owner;
	};

	const size_t OFFSET = sizeof(information);

	std::vector<ThreadHeap*> heaps;
	ThreadHeap* mainHeap;

	ThreadHeap* findOwner(Superblock* block) {
		int current = -1;
		ThreadHeap* currentHeap = mainHeap;
		bool isFirstIter = true;
		do {
			if (!isFirstIter) {
				if (current == -1) {
					mainHeap->operationMutex.unlock();
				}
				else {
					heaps[current]->operationMutex.unlock();
				}
			}
			current = block->ownerHeap;
			if (current == -1) {
				mainHeap->operationMutex.lock();
				currentHeap = mainHeap;
			}
			else {
				heaps[current]->operationMutex.lock();
				currentHeap = heaps[current];
			}
			isFirstIter = false;
		} while (current != block->ownerHeap);
		return currentHeap;
	}



	class MemoryController {
	public:

		static MemoryController& getInstance() {
			static MemoryController instance;
			return instance;
		}

		void remember(void* ptr) {
			pointers.insert(ptr);
		}

		void forget(void*  ptr) {
			pointers.erase(ptr);
		}

		~MemoryController() {
			for (auto ptr : pointers) {
				std::free(ptr);
			}
		}

	private:
		std::set<void*> pointers;
	};


};

extern void* mtalloc(size_t bytes) {
	return Controller::getInstance().allocate(bytes);
}

extern void mtfree(void* ptr) {
	Controller::getInstance().deallocate(ptr);
}
