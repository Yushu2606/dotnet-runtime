// Copyright (c) .NET Foundation and contributors. All rights reserved.
// Licensed under the MIT license. See LICENSE file in the project root for full license information.

using System;

namespace Mono.Linker.Tests.Cases.Expectations.Assertions
{
    [AttributeUsage(AttributeTargets.GenericParameter, Inherited = false)]
    public class KeptAttributeOnConstraintAttribute : KeptAttribute
    {
        public KeptAttributeOnConstraintAttribute(Type constraintType, Type attributeType)
        {
        }
    }
}
