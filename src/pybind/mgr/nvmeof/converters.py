# -*- coding: utf-8 -*-
import functools
import inspect
from typing import Annotated, Any, Callable, Dict, Generator, NamedTuple, \
    Optional, Type, get_args, get_origin

from google.protobuf.json_format import MessageToDict  # type: ignore
from google.protobuf.message import Message  # type: ignore

# protobuf 5 renamed the kwarg that keeps zero-valued fields in the dict
if 'always_print_fields_with_no_presence' in \
        inspect.signature(MessageToDict).parameters:
    _MESSAGE_TO_DICT_DEFAULTS = {'always_print_fields_with_no_presence': True}
else:
    _MESSAGE_TO_DICT_DEFAULTS = {'including_default_value_fields': True}

Model = Dict[str, Any]


class MaxRecursionDepthError(Exception):
    pass


def _convert(value: Any, field_type: Any, depth: int, max_depth: int) -> Generator:
    if depth > max_depth:
        raise MaxRecursionDepthError(
            f"Maximum nesting depth of {max_depth} exceeded at depth {depth}.")

    if isinstance(value, dict) and hasattr(field_type, '_fields'):
        # Lazily create NamedTuple for nested dicts
        yield from _lazily_create_namedtuple(value, field_type, depth + 1, max_depth)
    elif isinstance(value, list):
        # Handle empty lists directly
        if not value:
            yield []
        else:
            # Lazily process each item in the list based on the expected item type
            item_type = field_type.__args__[0] if hasattr(field_type, '__args__') else None
            processed_items = []
            for v in value:
                if item_type:
                    processed_items.append(next(_convert(v, item_type,
                                                         depth + 1, max_depth), None))
                else:
                    processed_items.append(v)
            yield processed_items
    else:
        # Yield the value as is for simple types
        yield value


def _lazily_create_namedtuple(data: Any, target_type: Type[NamedTuple],
                              depth: int, max_depth: int) -> Generator:
    # pylint: disable=protected-access
    """ Lazily create NamedTuple from a dict """
    field_values = {}
    for field, field_type in zip(target_type._fields,
                                 target_type.__annotations__.values()):
        if get_origin(field_type) == Annotated:
            field_type = get_args(field_type)[0]
        # these conditions are complex since we need to navigate between dicts,
        # empty dicts and objects
        if isinstance(data, dict) and data.get(field) is not None:
            try:
                field_values[field] = next(_convert(data.get(field), field_type,
                                                    depth, max_depth), None)
            except StopIteration:
                return
        elif hasattr(data, field):
            try:
                field_values[field] = next(_convert(getattr(data, field), field_type,
                                                    depth, max_depth), None)
            except StopIteration:
                return
        else:
            field_values[field] = target_type._field_defaults.get(field)

    namedtuple_instance = target_type(**field_values)  # type: ignore
    yield namedtuple_instance


def obj_to_namedtuple(data: Any, target_type: Type[NamedTuple],
                      max_depth: int = 7) -> NamedTuple:
    """
    Convert an object or dict to a NamedTuple, handling nesting and lists lazily.
    This will raise an error if nesting depth exceeds the max depth (default 4)
    to avoid bloating the memory in case of mutual references between objects.

    :param data: The input data - object or dictionary
    :param target_type: The target NamedTuple type
    :param max_depth: The maximum depth allowed for recursion
    :return: An instance of the target NamedTuple with fields populated from the JSON
    """

    if not isinstance(target_type, type) or not hasattr(target_type, '_fields'):
        raise TypeError("target_type must be a NamedTuple type.")
    if isinstance(data, list):
        raise TypeError("data can't be a list.")
    if data is None:
        raise TypeError("data can't be None.")
    namedtuple_values = next(_lazily_create_namedtuple(data, target_type, 1, max_depth))
    return namedtuple_values


def namedtuple_to_dict(obj: Any) -> Any:
    if isinstance(obj, tuple) and hasattr(obj, '_asdict'):
        # If it's a namedtuple, convert it to a dictionary
        return {k: namedtuple_to_dict(v) for k, v in obj._asdict().items()}
    if isinstance(obj, list):
        # If it's a list, check each item and convert if it's a namedtuple
        return [
            namedtuple_to_dict(item)
            if isinstance(item, tuple) and hasattr(item, '_asdict')
            else item
            for item in obj
        ]
    return obj


def convert_to_model(model: Type[NamedTuple],
                     finalize: Optional[Callable[[Dict], Dict]] = None
                     ) -> Callable[..., Callable[..., Model]]:
    def decorator(func: Callable[..., Message]) -> Callable[..., Model]:
        @functools.wraps(func)
        def wrapper(*args: Any, **kwargs: Any) -> Model:
            message = func(*args, **kwargs)
            msg_dict = MessageToDict(message, preserving_proto_field_name=True,
                                     **_MESSAGE_TO_DICT_DEFAULTS)  # type: ignore

            result = namedtuple_to_dict(obj_to_namedtuple(msg_dict, model))
            if finalize:
                return finalize(result)
            return result

        return wrapper

    return decorator
