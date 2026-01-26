"""
HFT RAG Service

Vector-based retrieval for HFT AI Tuner context enhancement.
Uses ChromaDB for storage and sentence-transformers for embeddings.

Endpoints:
  GET  /health          - Health check
  POST /query           - Query knowledge base
  POST /index           - Reindex documents
  GET  /stats           - Collection statistics
"""

import os
import re
import glob
from pathlib import Path
from typing import List, Optional
from datetime import datetime

from fastapi import FastAPI, HTTPException
from fastapi.middleware.cors import CORSMiddleware
from pydantic import BaseModel
import chromadb
from chromadb.config import Settings
from sentence_transformers import SentenceTransformer

# Configuration
PROJECT_ROOT = Path(__file__).parent.parent
KNOWLEDGE_DIR = PROJECT_ROOT / "knowledge"
INCLUDE_DIR = PROJECT_ROOT / "include"
CHROMA_DIR = PROJECT_ROOT / "rag_service" / "chroma_db"
EMBEDDING_MODEL = "all-MiniLM-L6-v2"  # Fast, good quality
COLLECTION_NAME = "hft_knowledge"
CHUNK_SIZE = 1000  # characters per chunk
CHUNK_OVERLAP = 200

# Initialize FastAPI
app = FastAPI(title="HFT RAG Service", version="1.0.0")
app.add_middleware(
    CORSMiddleware,
    allow_origins=["*"],
    allow_methods=["*"],
    allow_headers=["*"],
)

# Global instances
embedder: Optional[SentenceTransformer] = None
chroma_client: Optional[chromadb.Client] = None
collection: Optional[chromadb.Collection] = None


class QueryRequest(BaseModel):
    query: str
    regime: Optional[str] = None
    symbol: Optional[str] = None
    n_results: int = 5


class QueryResponse(BaseModel):
    context: str
    sources: List[str]
    n_chunks: int


class IndexResponse(BaseModel):
    status: str
    documents_indexed: int
    chunks_created: int


def init_components():
    """Initialize embedding model and ChromaDB."""
    global embedder, chroma_client, collection

    print(f"[RAG] Loading embedding model: {EMBEDDING_MODEL}")
    embedder = SentenceTransformer(EMBEDDING_MODEL)

    print(f"[RAG] Initializing ChromaDB at: {CHROMA_DIR}")
    CHROMA_DIR.mkdir(parents=True, exist_ok=True)
    chroma_client = chromadb.PersistentClient(path=str(CHROMA_DIR))

    # Get or create collection
    collection = chroma_client.get_or_create_collection(
        name=COLLECTION_NAME,
        metadata={"description": "HFT trading knowledge base"}
    )
    print(f"[RAG] Collection '{COLLECTION_NAME}' ready with {collection.count()} documents")


def chunk_text(text: str, source: str, chunk_size: int = CHUNK_SIZE, overlap: int = CHUNK_OVERLAP) -> List[dict]:
    """Split text into overlapping chunks."""
    chunks = []

    # Split by paragraphs first, then by size
    paragraphs = re.split(r'\n\n+', text)
    current_chunk = ""

    for para in paragraphs:
        if len(current_chunk) + len(para) < chunk_size:
            current_chunk += para + "\n\n"
        else:
            if current_chunk:
                chunks.append({
                    "text": current_chunk.strip(),
                    "source": source,
                    "type": "documentation"
                })
            current_chunk = para + "\n\n"

    if current_chunk:
        chunks.append({
            "text": current_chunk.strip(),
            "source": source,
            "type": "documentation"
        })

    return chunks


def extract_code_docs(file_path: Path) -> List[dict]:
    """Extract documentation and key code from C++ headers."""
    chunks = []
    content = file_path.read_text(encoding='utf-8', errors='ignore')

    # Extract class/struct documentation
    doc_pattern = r'/\*\*[\s\S]*?\*/\s*(class|struct)\s+(\w+)'
    for match in re.finditer(doc_pattern, content):
        doc_block = match.group(0)
        class_name = match.group(2)
        chunks.append({
            "text": f"## {class_name}\n\n{doc_block}",
            "source": str(file_path.relative_to(PROJECT_ROOT)),
            "type": "code_doc"
        })

    # Extract enum definitions (useful for regimes, signals)
    enum_pattern = r'enum\s+(?:class\s+)?(\w+)\s*\{([^}]+)\}'
    for match in re.finditer(enum_pattern, content):
        enum_name = match.group(1)
        enum_body = match.group(2)
        chunks.append({
            "text": f"## Enum: {enum_name}\n\n```cpp\nenum {enum_name} {{\n{enum_body}\n}}\n```",
            "source": str(file_path.relative_to(PROJECT_ROOT)),
            "type": "code_enum"
        })

    # Extract struct configs (important for parameters)
    config_pattern = r'struct\s+(\w*Config\w*)\s*\{([^}]+)\}'
    for match in re.finditer(config_pattern, content):
        config_name = match.group(1)
        config_body = match.group(2)
        chunks.append({
            "text": f"## Config: {config_name}\n\n```cpp\nstruct {config_name} {{\n{config_body}\n}}\n```",
            "source": str(file_path.relative_to(PROJECT_ROOT)),
            "type": "code_config"
        })

    return chunks


def index_documents() -> IndexResponse:
    """Index all documents from knowledge/ and include/."""
    all_chunks = []

    # Index markdown documentation
    for md_file in KNOWLEDGE_DIR.glob("**/*.md"):
        print(f"[RAG] Indexing: {md_file}")
        content = md_file.read_text(encoding='utf-8')
        chunks = chunk_text(content, str(md_file.relative_to(PROJECT_ROOT)))
        all_chunks.extend(chunks)

    # Index key C++ headers
    key_headers = [
        "include/strategy/regime_detector.hpp",
        "include/strategy/technical_indicators.hpp",
        "include/strategy/technical_indicators_strategy.hpp",
        "include/ipc/shared_config.hpp",
        "include/ipc/symbol_config.hpp",
        "include/risk/enhanced_risk_manager.hpp",
    ]

    for header in key_headers:
        header_path = PROJECT_ROOT / header
        if header_path.exists():
            print(f"[RAG] Extracting code docs from: {header}")
            chunks = extract_code_docs(header_path)
            all_chunks.extend(chunks)

    # Clear existing collection and add new documents
    if collection.count() > 0:
        # Delete all existing
        all_ids = collection.get()["ids"]
        if all_ids:
            collection.delete(ids=all_ids)

    # Add chunks to collection
    if all_chunks:
        ids = [f"chunk_{i}" for i in range(len(all_chunks))]
        texts = [c["text"] for c in all_chunks]
        metadatas = [{"source": c["source"], "type": c["type"]} for c in all_chunks]

        print(f"[RAG] Creating embeddings for {len(texts)} chunks...")
        embeddings = embedder.encode(texts).tolist()

        collection.add(
            ids=ids,
            embeddings=embeddings,
            documents=texts,
            metadatas=metadatas
        )

    return IndexResponse(
        status="success",
        documents_indexed=len(set(c["source"] for c in all_chunks)),
        chunks_created=len(all_chunks)
    )


@app.on_event("startup")
async def startup():
    init_components()
    # Auto-index on first start if empty
    if collection.count() == 0:
        print("[RAG] Collection empty, indexing documents...")
        result = index_documents()
        print(f"[RAG] Indexed {result.documents_indexed} docs, {result.chunks_created} chunks")


@app.get("/health")
async def health():
    return {
        "status": "healthy",
        "collection_size": collection.count() if collection else 0,
        "model": EMBEDDING_MODEL
    }


@app.post("/query", response_model=QueryResponse)
async def query(request: QueryRequest):
    """Query the knowledge base for relevant context."""
    if not collection or collection.count() == 0:
        raise HTTPException(status_code=503, detail="Knowledge base not indexed")

    # Build enhanced query with context
    query_text = request.query
    if request.regime:
        query_text += f" market regime: {request.regime}"
    if request.symbol:
        query_text += f" symbol: {request.symbol}"

    # Create query embedding
    query_embedding = embedder.encode([query_text]).tolist()[0]

    # Search
    results = collection.query(
        query_embeddings=[query_embedding],
        n_results=request.n_results
    )

    # Build context
    chunks = results["documents"][0] if results["documents"] else []
    sources = [m["source"] for m in results["metadatas"][0]] if results["metadatas"] else []

    context = "\n\n---\n\n".join(chunks)

    return QueryResponse(
        context=context,
        sources=list(set(sources)),
        n_chunks=len(chunks)
    )


@app.post("/index", response_model=IndexResponse)
async def reindex():
    """Reindex all documents."""
    return index_documents()


@app.get("/stats")
async def stats():
    """Get collection statistics."""
    return {
        "collection_name": COLLECTION_NAME,
        "total_chunks": collection.count() if collection else 0,
        "embedding_model": EMBEDDING_MODEL,
        "knowledge_dir": str(KNOWLEDGE_DIR),
        "indexed_at": datetime.now().isoformat()
    }


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=9528)
