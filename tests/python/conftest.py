import pytest
import tsq


@pytest.fixture(scope="session")
def small_data():
    return tsq.datagen.make_dataset(20_000, 15_000, seed=7)
